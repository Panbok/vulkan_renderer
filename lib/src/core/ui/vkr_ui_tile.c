#include "core/ui/vkr_ui_tile.h"

#include <math.h>

#define VKR_UI_TILE_FNV_OFFSET UINT64_C(14695981039346656037)
#define VKR_UI_TILE_FNV_PRIME UINT64_C(1099511628211)

typedef struct VkrUiTileRange {
  uint32_t min_column;
  uint32_t max_column;
  uint32_t min_row;
  uint32_t max_row;
  bool8_t valid;
} VkrUiTileRange;

typedef struct VkrUiTileCommandInfo {
  VkrUiTileRange range;
  uint64_t fingerprint;
} VkrUiTileCommandInfo;

static uint64_t vkr_ui_tile_hash_bytes(uint64_t hash, const void *data,
                                       uint64_t size) {
  const uint8_t *bytes = data;
  for (uint64_t i = 0u; i < size; ++i) {
    hash ^= bytes[i];
    hash *= VKR_UI_TILE_FNV_PRIME;
  }
  return hash;
}

static uint64_t
vkr_ui_tile_command_fingerprint(const VkrUiDrawCommand *command) {
  uint64_t hash = VKR_UI_TILE_FNV_OFFSET;
#define VKR_UI_TILE_HASH_FIELD(FIELD)                                          \
  hash = vkr_ui_tile_hash_bytes(hash, &command->FIELD, sizeof(command->FIELD))
  VKR_UI_TILE_HASH_FIELD(mode);
  VKR_UI_TILE_HASH_FIELD(rect_px);
  VKR_UI_TILE_HASH_FIELD(clip_rect_px);
  VKR_UI_TILE_HASH_FIELD(uv_rect);
  VKR_UI_TILE_HASH_FIELD(color);
  VKR_UI_TILE_HASH_FIELD(corner_radius_px);
  VKR_UI_TILE_HASH_FIELD(texture);
  VKR_UI_TILE_HASH_FIELD(screen_px_range);
  VKR_UI_TILE_HASH_FIELD(sdf_unit_range);
#undef VKR_UI_TILE_HASH_FIELD
  return hash;
}

static uint64_t vkr_ui_tile_hash_fingerprint(uint64_t hash,
                                             uint64_t fingerprint) {
  hash ^= fingerprint;
  return hash * VKR_UI_TILE_FNV_PRIME;
}

static VkrUiRect vkr_ui_tile_rect_dilate(VkrUiRect rect, float32_t amount) {
  return (VkrUiRect){rect.x - amount, rect.y - amount,
                     rect.width + amount * 2.0f, rect.height + amount * 2.0f};
}

static bool8_t vkr_ui_tile_rect_equal(VkrUiRect a, VkrUiRect b) {
  return MemCompare(&a, &b, sizeof(a)) == 0;
}

static VkrUiRect vkr_ui_tile_rect_union(VkrUiRect a, VkrUiRect b) {
  if (!vkr_ui_rect_has_area(a))
    return b;
  if (!vkr_ui_rect_has_area(b))
    return a;
  const float32_t right = Max(a.x + a.width, b.x + b.width);
  const float32_t bottom = Max(a.y + a.height, b.y + b.height);
  const float32_t x = Min(a.x, b.x);
  const float32_t y = Min(a.y, b.y);
  return (VkrUiRect){x, y, right - x, bottom - y};
}

VkrUiRect vkr_ui_tile_command_aabb(const VkrUiDrawCommand *command,
                                   uint32_t target_width,
                                   uint32_t target_height) {
  if (!command || target_width == 0u || target_height == 0u)
    return (VkrUiRect){0};
  VkrUiRect aabb =
      vkr_ui_rect_intersect(command->rect_px, command->clip_rect_px);
  if (command->mode == VKR_UI_DRAW_MODE_MTSDF_TEXT)
    aabb = vkr_ui_tile_rect_dilate(aabb, command->screen_px_range);
  return vkr_ui_rect_intersect(aabb,
                               (VkrUiRect){0.0f, 0.0f, (float32_t)target_width,
                                           (float32_t)target_height});
}

static VkrUiTileRange vkr_ui_tile_range(VkrUiRect aabb, uint32_t columns,
                                        uint32_t rows, uint32_t tile_size_px) {
  if (!vkr_ui_rect_has_area(aabb) || columns == 0u || rows == 0u ||
      tile_size_px == 0u)
    return (VkrUiTileRange){0};
  const float32_t right_pixel = ceilf(aabb.x + aabb.width) - 1.0f;
  const float32_t bottom_pixel = ceilf(aabb.y + aabb.height) - 1.0f;
  return (VkrUiTileRange){
      .min_column = Min((uint32_t)floorf(aabb.x) / tile_size_px, columns - 1u),
      .max_column =
          Min((uint32_t)Max(right_pixel, 0.0f) / tile_size_px, columns - 1u),
      .min_row = Min((uint32_t)floorf(aabb.y) / tile_size_px, rows - 1u),
      .max_row =
          Min((uint32_t)Max(bottom_pixel, 0.0f) / tile_size_px, rows - 1u),
      .valid = true_v,
  };
}

static bool8_t vkr_ui_tile_cache_resize(VkrUiTileCache *cache,
                                        uint32_t target_width,
                                        uint32_t target_height,
                                        uint32_t columns, uint32_t rows,
                                        uint32_t tile_size_px) {
  const uint64_t tile_count_64 = (uint64_t)columns * rows;
  if (!cache || !cache->retained_allocator || tile_count_64 == 0u ||
      tile_count_64 > UINT32_MAX)
    return false_v;
  const uint32_t tile_count = (uint32_t)tile_count_64;
  if (cache->previous_hashes && cache->column_count == columns &&
      cache->row_count == rows && cache->tile_size_px == tile_size_px &&
      cache->target_width == target_width &&
      cache->target_height == target_height)
    return true_v;
  uint64_t *hashes = vkr_allocator_alloc(cache->retained_allocator,
                                         tile_count_64 * sizeof(*hashes),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!hashes)
    return false_v;
  uint32_t *offsets = vkr_allocator_alloc(
      cache->retained_allocator, (tile_count_64 + 1u) * sizeof(*offsets),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!offsets) {
    vkr_allocator_free(cache->retained_allocator, hashes,
                       tile_count_64 * sizeof(*hashes),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }
  MemZero(hashes, tile_count_64 * sizeof(*hashes));
  if (cache->previous_hashes) {
    vkr_allocator_free(cache->retained_allocator, cache->previous_hashes,
                       (uint64_t)cache->tile_count * sizeof(*hashes),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_free(cache->retained_allocator, cache->tile_offsets,
                       ((uint64_t)cache->tile_count + 1u) * sizeof(*offsets),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  cache->previous_hashes = hashes;
  cache->tile_offsets = offsets;
  cache->column_count = columns;
  cache->row_count = rows;
  cache->tile_count = tile_count;
  cache->tile_size_px = tile_size_px;
  cache->target_width = target_width;
  cache->target_height = target_height;
  cache->bin_entry_count = 0u;
  cache->initialized = false_v;
  return true_v;
}

static bool8_t vkr_ui_tile_cache_reserve_indices(VkrUiTileCache *cache,
                                                 uint32_t required_count) {
  if (required_count <= cache->command_index_capacity)
    return true_v;
  uint32_t *indices = vkr_allocator_alloc(
      cache->retained_allocator, (uint64_t)required_count * sizeof(*indices),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!indices)
    return false_v;
  if (cache->command_indices)
    vkr_allocator_free(cache->retained_allocator, cache->command_indices,
                       (uint64_t)cache->command_index_capacity *
                           sizeof(*indices),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  cache->command_indices = indices;
  cache->command_index_capacity = required_count;
  return true_v;
}

void vkr_ui_tile_cache_init(VkrUiTileCache *cache,
                            VkrAllocator *retained_allocator) {
  if (!cache)
    return;
  *cache = (VkrUiTileCache){.retained_allocator = retained_allocator};
}

void vkr_ui_tile_cache_destroy(VkrUiTileCache *cache) {
  if (!cache)
    return;
  if (cache->previous_hashes && cache->retained_allocator)
    vkr_allocator_free(cache->retained_allocator, cache->previous_hashes,
                       (uint64_t)cache->tile_count *
                           sizeof(*cache->previous_hashes),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (cache->tile_offsets && cache->retained_allocator)
    vkr_allocator_free(cache->retained_allocator, cache->tile_offsets,
                       ((uint64_t)cache->tile_count + 1u) *
                           sizeof(*cache->tile_offsets),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (cache->command_indices && cache->retained_allocator)
    vkr_allocator_free(cache->retained_allocator, cache->command_indices,
                       (uint64_t)cache->command_index_capacity *
                           sizeof(*cache->command_indices),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  *cache = (VkrUiTileCache){0};
}

static void vkr_ui_tile_mark_range(uint8_t *dirty_tiles, uint32_t columns,
                                   VkrUiTileRange range) {
  if (!range.valid)
    return;
  for (uint32_t row = range.min_row; row <= range.max_row; ++row)
    for (uint32_t column = range.min_column; column <= range.max_column;
         ++column)
      dirty_tiles[row * columns + column] = 1u;
}

bool8_t
vkr_ui_tile_build(VkrUiTileCache *cache, VkrAllocator *frame_allocator,
                  uint32_t target_width, uint32_t target_height,
                  uint32_t tile_size_px, const VkrUiDrawCommand *commands,
                  uint32_t command_count, const VkrUiTileDamage *motion_damage,
                  uint32_t motion_damage_count, VkrUiTileFrame *out_frame) {
  if (!cache || !frame_allocator || !out_frame || target_width == 0u ||
      target_height == 0u || tile_size_px == 0u ||
      (command_count > 0u && !commands) ||
      (motion_damage_count > 0u && !motion_damage))
    return false_v;
  *out_frame = (VkrUiTileFrame){0};
  const uint32_t columns = (target_width + tile_size_px - 1u) / tile_size_px;
  const uint32_t rows = (target_height + tile_size_px - 1u) / tile_size_px;
  if (!vkr_ui_tile_cache_resize(cache, target_width, target_height, columns,
                                rows, tile_size_px))
    return false_v;
  const uint32_t tile_count = cache->tile_count;
  VkrUiTileCommandInfo *command_info =
      command_count
          ? vkr_allocator_alloc(frame_allocator,
                                (uint64_t)command_count * sizeof(*command_info),
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
          : NULL;
  uint8_t *dirty = vkr_allocator_alloc(frame_allocator, tile_count,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if ((command_count && !command_info) || !dirty)
    return false_v;
  MemZero(dirty, tile_count);

  uint64_t stream_hash = VKR_UI_TILE_FNV_OFFSET;
  for (uint32_t i = 0u; i < command_count; ++i) {
    command_info[i] = (VkrUiTileCommandInfo){
        .range = vkr_ui_tile_range(
            vkr_ui_tile_command_aabb(&commands[i], target_width, target_height),
            columns, rows, tile_size_px),
        .fingerprint = vkr_ui_tile_command_fingerprint(&commands[i]),
    };
    stream_hash =
        vkr_ui_tile_hash_fingerprint(stream_hash, command_info[i].fingerprint);
  }
  bool8_t has_motion_damage = false_v;
  for (uint32_t i = 0u; i < motion_damage_count; ++i)
    has_motion_damage |= !vkr_ui_tile_rect_equal(
        motion_damage[i].previous_aabb_px, motion_damage[i].current_aabb_px);
  if (cache->initialized && !has_motion_damage &&
      cache->previous_stream_hash == stream_hash) {
    *out_frame = (VkrUiTileFrame){
        .tile_offsets = cache->tile_offsets,
        .command_indices = cache->command_indices,
        .dirty_tiles = dirty,
        .column_count = columns,
        .row_count = rows,
        .tile_count = tile_count,
        .bin_entry_count = cache->bin_entry_count,
    };
    return true_v;
  }

  uint32_t *counts = vkr_allocator_alloc(frame_allocator,
                                         (uint64_t)tile_count * sizeof(*counts),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  uint64_t *hashes = vkr_allocator_alloc(frame_allocator,
                                         (uint64_t)tile_count * sizeof(*hashes),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!counts || !hashes)
    return false_v;
  MemZero(counts, (uint64_t)tile_count * sizeof(*counts));
  uint64_t entry_count_64 = 0u;
  for (uint32_t i = 0u; i < command_count; ++i) {
    const VkrUiTileRange range = command_info[i].range;
    if (!range.valid)
      continue;
    for (uint32_t row = range.min_row; row <= range.max_row; ++row)
      for (uint32_t column = range.min_column; column <= range.max_column;
           ++column) {
        counts[row * columns + column]++;
        entry_count_64++;
      }
  }
  if (entry_count_64 > UINT32_MAX)
    return false_v;
  const uint32_t entry_count = (uint32_t)entry_count_64;
  if (!vkr_ui_tile_cache_reserve_indices(cache, entry_count))
    return false_v;
  uint32_t *offsets = cache->tile_offsets;
  offsets[0] = 0u;
  for (uint32_t i = 0u; i < tile_count; ++i)
    offsets[i + 1u] = offsets[i] + counts[i];
  MemCopy(counts, offsets, (uint64_t)tile_count * sizeof(*counts));
  for (uint32_t tile = 0u; tile < tile_count; ++tile)
    hashes[tile] = VKR_UI_TILE_FNV_OFFSET;
  uint32_t *indices = cache->command_indices;
  for (uint32_t i = 0u; i < command_count; ++i) {
    const VkrUiTileRange range = command_info[i].range;
    if (!range.valid)
      continue;
    for (uint32_t row = range.min_row; row <= range.max_row; ++row)
      for (uint32_t column = range.min_column; column <= range.max_column;
           ++column) {
        const uint32_t tile = row * columns + column;
        indices[counts[tile]++] = i;
        hashes[tile] = vkr_ui_tile_hash_fingerprint(
            hashes[tile], command_info[i].fingerprint);
      }
  }

  for (uint32_t tile = 0u; tile < tile_count; ++tile) {
    dirty[tile] =
        !cache->initialized || hashes[tile] != cache->previous_hashes[tile];
  }
  for (uint32_t i = 0u; i < motion_damage_count; ++i) {
    const VkrUiTileDamage *damage = &motion_damage[i];
    if (vkr_ui_tile_rect_equal(damage->previous_aabb_px,
                               damage->current_aabb_px))
      continue;
    const VkrUiRect extent =
        vkr_ui_rect_intersect(vkr_ui_tile_rect_union(damage->previous_aabb_px,
                                                     damage->current_aabb_px),
                              (VkrUiRect){0.0f, 0.0f, (float32_t)target_width,
                                          (float32_t)target_height});
    vkr_ui_tile_mark_range(
        dirty, columns, vkr_ui_tile_range(extent, columns, rows, tile_size_px));
  }

  uint32_t dirty_count = 0u;
  for (uint32_t tile = 0u; tile < tile_count; ++tile)
    dirty_count += dirty[tile] != 0u;
  MemCopy(cache->previous_hashes, hashes,
          (uint64_t)tile_count * sizeof(*hashes));
  cache->previous_stream_hash = stream_hash;
  cache->bin_entry_count = entry_count;
  cache->initialized = true_v;
  *out_frame = (VkrUiTileFrame){
      .tile_offsets = offsets,
      .command_indices = indices,
      .dirty_tiles = dirty,
      .column_count = columns,
      .row_count = rows,
      .tile_count = tile_count,
      .bin_entry_count = entry_count,
      .dirty_tile_count = dirty_count,
      .dirty_tile_ratio = (float32_t)dirty_count / (float32_t)tile_count,
  };
  return true_v;
}
