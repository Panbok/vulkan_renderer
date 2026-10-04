#include "level/vkr_heightfield.h"

#include "filesystem/filesystem.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define HEIGHTFIELD_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
/* Version 1 holds tiles; version 2 adds the overview after them. */
#define HEIGHTFIELD_VERSION 2u

/* The file header; tiles follow, then (version 2) the overview's heights
   and weights. */
typedef struct HeightfieldHeader {
  char magic[8];
  uint32_t version;
  uint32_t cells;
  float32_t spacing;
  float32_t height_min;
  float32_t height_max;
  uint32_t tile_samples;
  uint32_t tiles_per_side;
  uint32_t overview_stride;
  uint32_t reserved[2];
} HeightfieldHeader;

static const char s_heightfield_magic[8] = {'V', 'K', 'R', 'H',
                                            'F', 'L', 'D', '1'};

static bool8_t heightfield_fail(char *error, uint32_t capacity,
                                const char *message) {
  if (error && capacity) {
    snprintf(error, capacity, "%s", message);
  }
  return false_v;
}

static uint32_t heightfield_tiles(uint32_t samples) {
  return (samples + VKR_HEIGHTFIELD_TILE_SAMPLES - 1u) /
         VKR_HEIGHTFIELD_TILE_SAMPLES;
}

static uint64_t heightfield_overview_count(uint32_t cells) {
  const uint64_t side = cells / VKR_HEIGHTFIELD_OVERVIEW_STRIDE + 1u;
  return side * side;
}

/* File offset of tile `index`; the overview follows the last tile. */
static uint64_t heightfield_tile_offset(uint32_t index) {
  return sizeof(HeightfieldHeader) +
         (uint64_t)index * sizeof(VkrHeightfieldTile);
}

static bool8_t heightfield_valid(uint32_t cells, float32_t spacing,
                                 float32_t height_min, float32_t height_max) {
  return cells && cells % VKR_HEIGHTFIELD_TILE_CELLS == 0u &&
         cells <= VKR_HEIGHTFIELD_CELLS_MAX &&
         (cells <= VKR_HEIGHTFIELD_RESIDENT_CELLS ||
          cells % VKR_HEIGHTFIELD_STREAMED_CELLS == 0u) &&
         spacing > 0.0f && isfinite(spacing) && isfinite(height_min) &&
         isfinite(height_max) && height_max > height_min;
}

static uint16_t *heightfield_height_ref(VkrHeightfield *field, uint32_t x,
                                        uint32_t z) {
  return &vkr_heightfield_tile_of(field, x, z)
              ->heights[vkr_heightfield_tile_index(x, z)];
}

static uint32_t *heightfield_weights_ref(VkrHeightfield *field, uint32_t x,
                                         uint32_t z) {
  return &vkr_heightfield_tile_of(field, x, z)
              ->weights[vkr_heightfield_tile_index(x, z)];
}

/* The tile pointer, dirty and overview arrays of a field of `cells`. */
static bool8_t heightfield_arrays(VkrHeightfield *field, uint32_t cells,
                                  float32_t spacing, float32_t height_min,
                                  float32_t height_max,
                                  VkrAllocator *allocator) {
  MemZero(field, sizeof(*field));
  field->cells = cells;
  field->spacing = spacing;
  field->height_min = height_min;
  field->height_max = height_max;
  field->allocator = allocator;
  field->tiles_per_side = heightfield_tiles(cells + 1u);
  const uint64_t tiles =
      (uint64_t)field->tiles_per_side * field->tiles_per_side;
  const uint64_t overview = heightfield_overview_count(cells);
  field->tiles = vkr_allocator_alloc(
      allocator, tiles * sizeof(VkrHeightfieldTile *), HEIGHTFIELD_TAG);
  field->tile_dirty =
      vkr_allocator_alloc(allocator, tiles * sizeof(uint8_t), HEIGHTFIELD_TAG);
  field->overview_heights = vkr_allocator_alloc(
      allocator, overview * sizeof(uint16_t), HEIGHTFIELD_TAG);
  field->overview_weights = vkr_allocator_alloc(
      allocator, overview * sizeof(uint32_t), HEIGHTFIELD_TAG);
  if (!field->tiles || !field->tile_dirty || !field->overview_heights ||
      !field->overview_weights) {
    vkr_heightfield_destroy(field, allocator);
    return false_v;
  }
  MemZero(field->tiles, tiles * sizeof(VkrHeightfieldTile *));
  MemZero(field->tile_dirty, tiles * sizeof(uint8_t));
  return true_v;
}

static VkrHeightfieldTile *heightfield_tile_new(VkrHeightfield *field,
                                                uint32_t index) {
  VkrHeightfieldTile *tile = vkr_allocator_alloc(
      field->allocator, sizeof(VkrHeightfieldTile), HEIGHTFIELD_TAG);
  if (tile) {
    field->tiles[index] = tile;
    field->resident_tiles++;
  }
  return tile;
}

static void heightfield_tile_free(VkrHeightfield *field, uint32_t index) {
  vkr_allocator_free(field->allocator, field->tiles[index],
                     sizeof(VkrHeightfieldTile), HEIGHTFIELD_TAG);
  field->tiles[index] = NULL;
  field->tile_dirty[index] = 0u;
  field->resident_tiles--;
}

static void heightfield_tile_fill(VkrHeightfieldTile *tile, uint16_t height) {
  for (uint32_t i = 0; i < ArrayCount(tile->heights); ++i) {
    tile->heights[i] = height;
    tile->weights[i] = 0xFFu;
  }
}

/* Copies resident samples at overview positions inside `rect` into the
   overview. */
static void heightfield_overview_refresh(VkrHeightfield *field,
                                         VkrHeightfieldRect rect) {
  const uint32_t stride = VKR_HEIGHTFIELD_OVERVIEW_STRIDE;
  const uint32_t side = vkr_heightfield_overview_samples(field);
  for (uint32_t oz = (rect.z0 + stride - 1u) / stride; oz * stride <= rect.z1;
       ++oz) {
    for (uint32_t ox = (rect.x0 + stride - 1u) / stride; ox * stride <= rect.x1;
         ++ox) {
      const uint32_t x = ox * stride;
      const uint32_t z = oz * stride;
      if (vkr_heightfield_tile_of(field, x, z)) {
        field->overview_heights[oz * side + ox] =
            vkr_heightfield_raw(field, x, z);
        field->overview_weights[oz * side + ox] =
            vkr_heightfield_weights_at(field, x, z);
      }
    }
  }
}

/* Marks the tiles of `rect` dirty and refreshes its overview samples. */
static void heightfield_mark(VkrHeightfield *field, VkrHeightfieldRect rect) {
  const uint32_t size = VKR_HEIGHTFIELD_TILE_SAMPLES;
  for (uint32_t tz = rect.z0 / size; tz <= rect.z1 / size; ++tz) {
    for (uint32_t tx = rect.x0 / size; tx <= rect.x1 / size; ++tx) {
      field->tile_dirty[tz * field->tiles_per_side + tx] = 1u;
    }
  }
  heightfield_overview_refresh(field, rect);
}

bool8_t vkr_heightfield_create(VkrHeightfield *out, uint32_t cells,
                               float32_t spacing, float32_t height_min,
                               float32_t height_max, float32_t height,
                               VkrAllocator *allocator) {
  MemZero(out, sizeof(*out));
  if (!heightfield_valid(cells, spacing, height_min, height_max) ||
      cells > VKR_HEIGHTFIELD_RESIDENT_CELLS ||
      !heightfield_arrays(out, cells, spacing, height_min, height_max,
                          allocator)) {
    return false_v;
  }
  const uint16_t value = vkr_heightfield_quantize(out, height);
  const uint32_t tiles = out->tiles_per_side * out->tiles_per_side;
  for (uint32_t i = 0; i < tiles; ++i) {
    VkrHeightfieldTile *tile = heightfield_tile_new(out, i);
    if (!tile) {
      vkr_heightfield_destroy(out, allocator);
      return false_v;
    }
    heightfield_tile_fill(tile, value);
  }
  const uint64_t overview = heightfield_overview_count(cells);
  for (uint64_t i = 0; i < overview; ++i) {
    out->overview_heights[i] = value;
    out->overview_weights[i] = 0xFFu;
  }
  return true_v;
}

void vkr_heightfield_destroy(VkrHeightfield *field, VkrAllocator *allocator) {
  const uint64_t tiles =
      (uint64_t)field->tiles_per_side * field->tiles_per_side;
  const uint64_t overview = heightfield_overview_count(field->cells);
  if (field->tiles) {
    for (uint64_t i = 0; i < tiles; ++i) {
      if (field->tiles[i]) {
        vkr_allocator_free(allocator, field->tiles[i],
                           sizeof(VkrHeightfieldTile), HEIGHTFIELD_TAG);
      }
    }
    vkr_allocator_free(allocator, field->tiles,
                       tiles * sizeof(VkrHeightfieldTile *), HEIGHTFIELD_TAG);
  }
  if (field->tile_dirty) {
    vkr_allocator_free(allocator, field->tile_dirty, tiles * sizeof(uint8_t),
                       HEIGHTFIELD_TAG);
  }
  if (field->overview_heights) {
    vkr_allocator_free(allocator, field->overview_heights,
                       overview * sizeof(uint16_t), HEIGHTFIELD_TAG);
  }
  if (field->overview_weights) {
    vkr_allocator_free(allocator, field->overview_weights,
                       overview * sizeof(uint32_t), HEIGHTFIELD_TAG);
  }
  if (field->file) {
    fclose((FILE *)field->file);
  }
  MemZero(field, sizeof(*field));
}

float32_t vkr_heightfield_metres(const VkrHeightfield *field, uint16_t value) {
  return field->height_min + (field->height_max - field->height_min) *
                                 ((float32_t)value / 65535.0f);
}

uint16_t vkr_heightfield_quantize(const VkrHeightfield *field,
                                  float32_t metres) {
  const float32_t t =
      (metres - field->height_min) / (field->height_max - field->height_min);
  const float32_t clamped = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
  return (uint16_t)lroundf(clamped * 65535.0f);
}

float32_t vkr_heightfield_at(const VkrHeightfield *field, uint32_t x,
                             uint32_t z) {
  return vkr_heightfield_metres(field, vkr_heightfield_raw(field, x, z));
}

float32_t vkr_heightfield_overview_at(const VkrHeightfield *field, uint32_t x,
                                      uint32_t z) {
  return vkr_heightfield_metres(
      field,
      field->overview_heights[z * vkr_heightfield_overview_samples(field) + x]);
}

bool8_t vkr_heightfield_sample(const VkrHeightfield *field, float32_t x,
                               float32_t z, float32_t *out_height) {
  const float32_t half = vkr_heightfield_half_size(field);
  float32_t gx = (x + half) / field->spacing;
  float32_t gz = (z + half) / field->spacing;
  if (!(gx >= 0.0f) || !(gz >= 0.0f) || gx > (float32_t)field->cells ||
      gz > (float32_t)field->cells) {
    return false_v;
  }
  uint32_t x0 = Min((uint32_t)gx, field->cells - 1u);
  uint32_t z0 = Min((uint32_t)gz, field->cells - 1u);
  const bool8_t resident = vkr_heightfield_tile_of(field, x0, z0) &&
                           vkr_heightfield_tile_of(field, x0 + 1u, z0) &&
                           vkr_heightfield_tile_of(field, x0, z0 + 1u) &&
                           vkr_heightfield_tile_of(field, x0 + 1u, z0 + 1u);
  float32_t h00, h10, h01, h11;
  if (resident) {
    h00 = vkr_heightfield_at(field, x0, z0);
    h10 = vkr_heightfield_at(field, x0 + 1u, z0);
    h01 = vkr_heightfield_at(field, x0, z0 + 1u);
    h11 = vkr_heightfield_at(field, x0 + 1u, z0 + 1u);
  } else {
    /* Unloaded ground answers from the overview. */
    const uint32_t last = vkr_heightfield_overview_samples(field) - 1u;
    gx /= (float32_t)VKR_HEIGHTFIELD_OVERVIEW_STRIDE;
    gz /= (float32_t)VKR_HEIGHTFIELD_OVERVIEW_STRIDE;
    x0 = Min((uint32_t)gx, last - 1u);
    z0 = Min((uint32_t)gz, last - 1u);
    h00 = vkr_heightfield_overview_at(field, x0, z0);
    h10 = vkr_heightfield_overview_at(field, x0 + 1u, z0);
    h01 = vkr_heightfield_overview_at(field, x0, z0 + 1u);
    h11 = vkr_heightfield_overview_at(field, x0 + 1u, z0 + 1u);
  }
  const float32_t fx = gx - (float32_t)x0;
  const float32_t fz = gz - (float32_t)z0;
  *out_height = (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fz) +
                (h01 * (1.0f - fx) + h11 * fx) * fz;
  return true_v;
}

bool8_t vkr_heightfield_resident(const VkrHeightfield *field,
                                 VkrHeightfieldRect rect) {
  const uint32_t size = VKR_HEIGHTFIELD_TILE_SAMPLES;
  for (uint32_t tz = rect.z0 / size; tz <= rect.z1 / size; ++tz) {
    for (uint32_t tx = rect.x0 / size; tx <= rect.x1 / size; ++tx) {
      if (!vkr_heightfield_tile(field, tx, tz)) {
        return false_v;
      }
    }
  }
  return true_v;
}

/* The samples covering local [min, max] metres, clipped. */
static bool8_t heightfield_rect(const VkrHeightfield *field, Vec2 min, Vec2 max,
                                VkrHeightfieldRect *out) {
  const float32_t half = vkr_heightfield_half_size(field);
  const float32_t last = (float32_t)field->cells;
  const float32_t x0 = ceilf((min.x + half) / field->spacing);
  const float32_t z0 = ceilf((min.y + half) / field->spacing);
  const float32_t x1 = floorf((max.x + half) / field->spacing);
  const float32_t z1 = floorf((max.y + half) / field->spacing);
  if (!(x1 >= 0.0f) || !(z1 >= 0.0f) || !(x0 <= last) || !(z0 <= last) ||
      x0 > x1 || z0 > z1) {
    return false_v;
  }
  *out = (VkrHeightfieldRect){
      .x0 = (uint32_t)Max(0.0f, x0),
      .z0 = (uint32_t)Max(0.0f, z0),
      .x1 = (uint32_t)Min(last, x1),
      .z1 = (uint32_t)Min(last, z1),
  };
  return true_v;
}

bool8_t vkr_heightfield_rect_around(const VkrHeightfield *field, float32_t x,
                                    float32_t z, float32_t radius,
                                    VkrHeightfieldRect *out) {
  return heightfield_rect(field, vec2_new(x - radius, z - radius),
                          vec2_new(x + radius, z + radius), out);
}

bool8_t vkr_heightfield_rect_union(const VkrHeightfield *field,
                                   VkrHeightfieldRect a, VkrHeightfieldRect b,
                                   VkrHeightfieldRect *out) {
  (void)field;
  *out = (VkrHeightfieldRect){.x0 = Min(a.x0, b.x0),
                              .z0 = Min(a.z0, b.z0),
                              .x1 = Max(a.x1, b.x1),
                              .z1 = Max(a.z1, b.z1)};
  return true_v;
}

void vkr_heightfield_read_rect(const VkrHeightfield *field,
                               VkrHeightfieldRect rect, uint16_t *heights,
                               uint32_t *weights) {
  const uint32_t width = rect.x1 - rect.x0 + 1u;
  for (uint32_t z = rect.z0; z <= rect.z1; ++z) {
    for (uint32_t x = rect.x0; x <= rect.x1; ++x) {
      const size_t to = (size_t)(z - rect.z0) * width + (x - rect.x0);
      heights[to] = vkr_heightfield_raw(field, x, z);
      weights[to] = vkr_heightfield_weights_at(field, x, z);
    }
  }
}

void vkr_heightfield_write_rect(VkrHeightfield *field, VkrHeightfieldRect rect,
                                const uint16_t *heights,
                                const uint32_t *weights) {
  const uint32_t width = rect.x1 - rect.x0 + 1u;
  for (uint32_t z = rect.z0; z <= rect.z1; ++z) {
    for (uint32_t x = rect.x0; x <= rect.x1; ++x) {
      const size_t from = (size_t)(z - rect.z0) * width + (x - rect.x0);
      *heightfield_height_ref(field, x, z) = heights[from];
      *heightfield_weights_ref(field, x, z) = weights[from];
    }
  }
  heightfield_mark(field, rect);
}

// =============================================================================
// Operations
// =============================================================================

static float32_t heightfield_smooth01(float32_t t) {
  t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
  return t * t * (3.0f - 2.0f * t);
}

static Vec2 heightfield_local(const VkrHeightfield *field, uint32_t x,
                              uint32_t z) {
  const float32_t half = vkr_heightfield_half_size(field);
  return vec2_new((float32_t)x * field->spacing - half,
                  (float32_t)z * field->spacing - half);
}

/* Moves sample (x, z) toward `target` metres by `t`. */
static void heightfield_blend(VkrHeightfield *field, uint32_t x, uint32_t z,
                              float32_t target, float32_t t) {
  uint16_t *height = heightfield_height_ref(field, x, z);
  const float32_t now = vkr_heightfield_metres(field, *height);
  *height = vkr_heightfield_quantize(field, now + (target - now) * t);
}

/* Adds `t` of layer `layer` to sample (x, z), keeping weights summing to
   255. */
static void heightfield_paint(VkrHeightfield *field, uint32_t x, uint32_t z,
                              uint32_t layer, float32_t t) {
  uint32_t *weights = heightfield_weights_ref(field, x, z);
  float32_t w[VKR_HEIGHTFIELD_LAYERS];
  float32_t sum = 0.0f;
  for (uint32_t i = 0; i < VKR_HEIGHTFIELD_LAYERS; ++i) {
    w[i] = (float32_t)((*weights >> (8u * i)) & 0xFFu);
    sum += w[i];
  }
  for (uint32_t i = 0; i < VKR_HEIGHTFIELD_LAYERS; ++i) {
    w[i] = sum > 0.0f ? w[i] / sum : (i == 0u ? 1.0f : 0.0f);
    w[i] = i == layer ? w[i] + t * (1.0f - w[i]) : w[i] * (1.0f - t);
  }
  uint32_t bytes[VKR_HEIGHTFIELD_LAYERS];
  uint32_t total = 0u;
  for (uint32_t i = 0; i < VKR_HEIGHTFIELD_LAYERS; ++i) {
    bytes[i] = (uint32_t)lroundf(w[i] * 255.0f);
    total += bytes[i];
  }
  /* Rounding drift goes to the painted layer. */
  bytes[layer] = (uint32_t)((int32_t)bytes[layer] + 255 - (int32_t)total);
  *weights =
      bytes[0] | (bytes[1] << 8u) | (bytes[2] << 16u) | (bytes[3] << 24u);
}

bool8_t vkr_heightfield_brush(VkrHeightfield *field, VkrHeightfieldBrush brush,
                              float32_t x, float32_t z, float32_t radius,
                              float32_t strength, float32_t height,
                              uint32_t layer, VkrAllocator *scratch,
                              VkrHeightfieldRect *touched) {
  if (!(radius > 0.0f) || !isfinite(strength) || !isfinite(height) ||
      brush >= VKR_HEIGHTFIELD_BRUSH_COUNT || layer >= VKR_HEIGHTFIELD_LAYERS ||
      !vkr_heightfield_rect_around(field, x, z, radius, touched)) {
    return false_v;
  }
  const uint32_t samples = vkr_heightfield_samples(field);
  /* Smoothing writes the touched heights only after reading all of them. */
  const uint64_t staged_bytes =
      sizeof(uint16_t) * vkr_heightfield_rect_count(*touched);
  uint16_t *staged = NULL;
  if (brush == VKR_HEIGHTFIELD_SMOOTH) {
    staged = vkr_allocator_alloc(scratch, staged_bytes, HEIGHTFIELD_TAG);
    if (!staged) {
      return false_v;
    }
  }
  const uint32_t width = touched->x1 - touched->x0 + 1u;
  for (uint32_t sz = touched->z0; sz <= touched->z1; ++sz) {
    for (uint32_t sx = touched->x0; sx <= touched->x1; ++sx) {
      const Vec2 at = heightfield_local(field, sx, sz);
      const float32_t d = vec2_length(vec2_new(at.x - x, at.y - z));
      const float32_t w = heightfield_smooth01(1.0f - d / radius);
      if (w <= 0.0f) {
        continue;
      }
      const float32_t now =
          vkr_heightfield_metres(field, vkr_heightfield_raw(field, sx, sz));
      switch (brush) {
      case VKR_HEIGHTFIELD_RAISE:
        heightfield_blend(field, sx, sz, now + strength, w);
        break;
      case VKR_HEIGHTFIELD_LOWER:
        heightfield_blend(field, sx, sz, now - strength, w);
        break;
      case VKR_HEIGHTFIELD_SMOOTH: {
        float32_t sum = 0.0f;
        uint32_t count = 0u;
        for (int32_t dz = -1; dz <= 1; ++dz) {
          for (int32_t dx = -1; dx <= 1; ++dx) {
            const int32_t nx = (int32_t)sx + dx;
            const int32_t nz = (int32_t)sz + dz;
            if (nx >= 0 && nz >= 0 && nx < (int32_t)samples &&
                nz < (int32_t)samples) {
              sum += vkr_heightfield_at(field, (uint32_t)nx, (uint32_t)nz);
              count++;
            }
          }
        }
        const float32_t t = Min(1.0f, Max(0.0f, strength)) * w;
        staged[(size_t)(sz - touched->z0) * width + (sx - touched->x0)] =
            vkr_heightfield_quantize(field,
                                     now + (sum / (float32_t)count - now) * t);
        break;
      }
      case VKR_HEIGHTFIELD_FLATTEN:
        heightfield_blend(field, sx, sz, height, strength * w);
        break;
      case VKR_HEIGHTFIELD_PAINT:
        heightfield_paint(field, sx, sz, layer,
                          Min(1.0f, Max(0.0f, strength)) * w);
        break;
      default:
        break;
      }
    }
  }
  if (staged) {
    /* Untouched samples (outside the radius) keep their heights. */
    for (uint32_t sz = touched->z0; sz <= touched->z1; ++sz) {
      for (uint32_t sx = touched->x0; sx <= touched->x1; ++sx) {
        const Vec2 at = heightfield_local(field, sx, sz);
        if (vec2_length(vec2_new(at.x - x, at.y - z)) < radius) {
          *heightfield_height_ref(field, sx, sz) =
              staged[(size_t)(sz - touched->z0) * width + (sx - touched->x0)];
        }
      }
    }
    vkr_allocator_free(scratch, staged, staged_bytes, HEIGHTFIELD_TAG);
  }
  return true_v;
}

bool8_t vkr_heightfield_flatten(VkrHeightfield *field, Vec2 min, Vec2 max,
                                float32_t height, float32_t falloff,
                                VkrHeightfieldRect *touched) {
  falloff = Max(0.0f, falloff);
  if (!isfinite(height) || !isfinite(falloff) || min.x > max.x ||
      min.y > max.y ||
      !heightfield_rect(field, vec2_new(min.x - falloff, min.y - falloff),
                        vec2_new(max.x + falloff, max.y + falloff), touched)) {
    return false_v;
  }
  for (uint32_t sz = touched->z0; sz <= touched->z1; ++sz) {
    for (uint32_t sx = touched->x0; sx <= touched->x1; ++sx) {
      const Vec2 at = heightfield_local(field, sx, sz);
      const float32_t dx = Max(0.0f, Max(min.x - at.x, at.x - max.x));
      const float32_t dz = Max(0.0f, Max(min.y - at.y, at.y - max.y));
      const float32_t d = sqrtf(dx * dx + dz * dz);
      const float32_t t =
          d <= 0.0f ? 1.0f
                    : (falloff > 0.0f ? 1.0f - heightfield_smooth01(d / falloff)
                                      : 0.0f);
      if (t > 0.0f) {
        heightfield_blend(field, sx, sz, height, t);
      }
    }
  }
  return true_v;
}

bool8_t vkr_heightfield_ramp(VkrHeightfield *field, Vec3 a, Vec3 b,
                             float32_t width, float32_t falloff,
                             VkrHeightfieldRect *touched) {
  falloff = Max(0.0f, falloff);
  const Vec2 a2 = vec2_new(a.x, a.z);
  const Vec2 b2 = vec2_new(b.x, b.z);
  const Vec2 along = vec2_sub(b2, a2);
  const float32_t length_sq = vec2_dot(along, along);
  const float32_t reach = 0.5f * width + falloff;
  if (!(width > 0.0f) || !(length_sq > 1.0e-6f) || !isfinite(a.y) ||
      !isfinite(b.y) ||
      !heightfield_rect(
          field, vec2_new(Min(a.x, b.x) - reach, Min(a.z, b.z) - reach),
          vec2_new(Max(a.x, b.x) + reach, Max(a.z, b.z) + reach), touched)) {
    return false_v;
  }
  for (uint32_t sz = touched->z0; sz <= touched->z1; ++sz) {
    for (uint32_t sx = touched->x0; sx <= touched->x1; ++sx) {
      const Vec2 at = heightfield_local(field, sx, sz);
      float32_t s = vec2_dot(vec2_sub(at, a2), along) / length_sq;
      s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
      const Vec2 closest = vec2_add(a2, vec2_scale(along, s));
      const float32_t d = vec2_length(vec2_sub(at, closest)) - 0.5f * width;
      const float32_t t =
          d <= 0.0f ? 1.0f
                    : (falloff > 0.0f ? 1.0f - heightfield_smooth01(d / falloff)
                                      : 0.0f);
      if (t > 0.0f) {
        heightfield_blend(field, sx, sz, a.y + (b.y - a.y) * s, t);
      }
    }
  }
  return true_v;
}

bool8_t vkr_heightfield_op_rect(const VkrHeightfield *field,
                                const VkrHeightfieldOp *op,
                                VkrHeightfieldRect *out) {
  switch (op->kind) {
  case VKR_HEIGHTFIELD_OP_BRUSH:
    return op->radius > 0.0f && vkr_heightfield_rect_around(
                                    field, op->a.x, op->a.z, op->radius, out);
  case VKR_HEIGHTFIELD_OP_STAMP:
    return op->image && op->image_width >= 2u && op->image_height >= 2u &&
           op->min.x < op->max.x && op->min.y < op->max.y &&
           heightfield_rect(field, op->min, op->max, out);
  case VKR_HEIGHTFIELD_OP_FLATTEN: {
    const float32_t falloff = Max(0.0f, op->falloff);
    return op->min.x <= op->max.x && op->min.y <= op->max.y &&
           heightfield_rect(
               field, vec2_new(op->min.x - falloff, op->min.y - falloff),
               vec2_new(op->max.x + falloff, op->max.y + falloff), out);
  }
  case VKR_HEIGHTFIELD_OP_RAMP: {
    const float32_t reach = 0.5f * op->width + Max(0.0f, op->falloff);
    return heightfield_rect(
        field,
        vec2_new(Min(op->a.x, op->b.x) - reach, Min(op->a.z, op->b.z) - reach),
        vec2_new(Max(op->a.x, op->b.x) + reach, Max(op->a.z, op->b.z) + reach),
        out);
  }
  case VKR_HEIGHTFIELD_OP_ROAD: {
    if (!op->path || op->path_count < 2u || !(op->width > 0.0f)) {
      return false_v;
    }
    const float32_t reach = 0.5f * op->width + Max(0.0f, op->falloff);
    Vec2 lo = vec2_new(INFINITY, INFINITY);
    Vec2 hi = vec2_new(-INFINITY, -INFINITY);
    for (uint32_t i = 0; i < op->path_count; ++i) {
      const Vec3 p = vec3_add(op->path[i], op->a);
      lo = vec2_new(Min(lo.x, p.x), Min(lo.y, p.z));
      hi = vec2_new(Max(hi.x, p.x), Max(hi.y, p.z));
    }
    return isfinite(lo.x) && isfinite(lo.y) && isfinite(hi.x) &&
           isfinite(hi.y) &&
           heightfield_rect(field, vec2_new(lo.x - reach, lo.y - reach),
                            vec2_new(hi.x + reach, hi.y + reach), out);
  }
  default:
    return false_v;
  }
}

static bool8_t heightfield_op_run(VkrHeightfield *field,
                                  const VkrHeightfieldOp *op,
                                  VkrAllocator *scratch,
                                  VkrHeightfieldRect *touched) {
  switch (op->kind) {
  case VKR_HEIGHTFIELD_OP_BRUSH:
    return vkr_heightfield_brush(field, op->brush, op->a.x, op->a.z, op->radius,
                                 op->strength, op->height, op->layer, scratch,
                                 touched);
  case VKR_HEIGHTFIELD_OP_FLATTEN:
    return vkr_heightfield_flatten(field, op->min, op->max, op->height,
                                   op->falloff, touched);
  case VKR_HEIGHTFIELD_OP_RAMP:
    return vkr_heightfield_ramp(field, op->a, op->b, op->width, op->falloff,
                                touched);
  case VKR_HEIGHTFIELD_OP_STAMP: {
    if (!vkr_heightfield_op_rect(field, op, touched) || !isfinite(op->height) ||
        !isfinite(op->strength)) {
      return false_v;
    }
    const float32_t w = (float32_t)(op->image_width - 1u);
    const float32_t h = (float32_t)(op->image_height - 1u);
    for (uint32_t sz = touched->z0; sz <= touched->z1; ++sz) {
      for (uint32_t sx = touched->x0; sx <= touched->x1; ++sx) {
        const Vec2 at = heightfield_local(field, sx, sz);
        /* Bilinear image value at this sample. */
        const float32_t u = (at.x - op->min.x) / (op->max.x - op->min.x) * w;
        const float32_t v = (at.y - op->min.y) / (op->max.y - op->min.y) * h;
        const uint32_t u0 = Min((uint32_t)Max(0.0f, u), op->image_width - 2u);
        const uint32_t v0 = Min((uint32_t)Max(0.0f, v), op->image_height - 2u);
        const float32_t fu = Min(1.0f, Max(0.0f, u - (float32_t)u0));
        const float32_t fv = Min(1.0f, Max(0.0f, v - (float32_t)v0));
        const float32_t *row0 = op->image + (size_t)v0 * op->image_width;
        const float32_t *row1 = row0 + op->image_width;
        const float32_t value =
            (row0[u0] * (1.0f - fu) + row0[u0 + 1u] * fu) * (1.0f - fv) +
            (row1[u0] * (1.0f - fu) + row1[u0 + 1u] * fu) * fv;
        const float32_t now = vkr_heightfield_at(field, sx, sz);
        heightfield_blend(field, sx, sz,
                          op->add ? now + value * op->strength
                                  : op->height + value * op->strength,
                          1.0f);
      }
    }
    return true_v;
  }
  case VKR_HEIGHTFIELD_OP_ROAD: {
    if (!vkr_heightfield_op_rect(field, op, touched)) {
      return false_v;
    }
    /* Segments run in order, so a later one shapes the samples two share. */
    for (uint32_t i = 0; i + 1u < op->path_count; ++i) {
      VkrHeightfieldRect segment;
      (void)vkr_heightfield_ramp(field, vec3_add(op->path[i], op->a),
                                 vec3_add(op->path[i + 1u], op->a), op->width,
                                 op->falloff, &segment);
    }
    return true_v;
  }
  default:
    return false_v;
  }
}

bool8_t vkr_heightfield_op_apply(VkrHeightfield *field,
                                 const VkrHeightfieldOp *op,
                                 VkrAllocator *scratch,
                                 VkrHeightfieldRect *touched) {
  /* The samples the op writes, and the neighbours smoothing reads. */
  VkrHeightfieldRect reach;
  if (!vkr_heightfield_op_rect(field, op, &reach)) {
    return false_v;
  }
  reach.x0 = reach.x0 ? reach.x0 - 1u : 0u;
  reach.z0 = reach.z0 ? reach.z0 - 1u : 0u;
  reach.x1 = Min(reach.x1 + 1u, field->cells);
  reach.z1 = Min(reach.z1 + 1u, field->cells);
  if (!vkr_heightfield_load_rect(field, reach) ||
      !heightfield_op_run(field, op, scratch, touched)) {
    return false_v;
  }
  heightfield_mark(field, *touched);
  return true_v;
}

// =============================================================================
// Files
// =============================================================================

static bool8_t heightfield_write_header(FILE *file, uint32_t cells,
                                        float32_t spacing, float32_t height_min,
                                        float32_t height_max) {
  HeightfieldHeader header = {.version = HEIGHTFIELD_VERSION,
                              .cells = cells,
                              .spacing = spacing,
                              .height_min = height_min,
                              .height_max = height_max,
                              .tile_samples = VKR_HEIGHTFIELD_TILE_SAMPLES,
                              .tiles_per_side = heightfield_tiles(cells + 1u),
                              .overview_stride =
                                  VKR_HEIGHTFIELD_OVERVIEW_STRIDE};
  MemCopy(header.magic, s_heightfield_magic, sizeof(header.magic));
  return fwrite(&header, sizeof(header), 1u, file) == 1u;
}

/* Replaces `path` with the written temporary file `temp`. */
static bool8_t heightfield_commit(const char *temp, const char *path,
                                  bool8_t ok, char *error, uint32_t capacity) {
  FilePath from = {
      .path = string8_create_from_cstr((const uint8_t *)temp, strlen(temp)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  FilePath to = {
      .path = string8_create_from_cstr((const uint8_t *)path, strlen(path)),
      .type = FILE_PATH_TYPE_ABSOLUTE};
  if (!ok || file_rename(&from, &to, true_v) != FILE_ERROR_NONE) {
    (void)file_remove(&from);
    return heightfield_fail(error, capacity,
                            "The terrain file could not be written");
  }
  return true_v;
}

bool8_t vkr_heightfield_create_file(const char *path, uint32_t cells,
                                    float32_t spacing, float32_t height_min,
                                    float32_t height_max, float32_t height,
                                    char *error, uint32_t capacity) {
  if (!heightfield_valid(cells, spacing, height_min, height_max)) {
    return heightfield_fail(error, capacity, "The terrain size is invalid");
  }
  char temp[1100];
  if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) {
    return heightfield_fail(error, capacity, "The terrain path is too long");
  }
  FILE *file = file_fopen(temp, "wb");
  if (!file) {
    return heightfield_fail(error, capacity,
                            "The terrain file could not be created");
  }
  VkrHeightfield shape = {.height_min = height_min, .height_max = height_max};
  const uint16_t value = vkr_heightfield_quantize(&shape, height);
  VkrHeightfieldTile tile;
  heightfield_tile_fill(&tile, value);
  const uint32_t tiles = heightfield_tiles(cells + 1u);
  bool8_t ok =
      heightfield_write_header(file, cells, spacing, height_min, height_max);
  for (uint32_t i = 0; ok && i < tiles * tiles; ++i) {
    ok = fwrite(&tile, sizeof(tile), 1u, file) == 1u;
  }
  /* The overview, heights then weights, a tile's worth at a time. */
  const uint64_t overview = heightfield_overview_count(cells);
  const uint32_t chunk = ArrayCount(tile.heights);
  for (uint64_t i = 0; ok && i < overview; i += chunk) {
    const size_t count = (size_t)Min((uint64_t)chunk, overview - i);
    ok = fwrite(tile.heights, sizeof(uint16_t), count, file) == count;
  }
  for (uint64_t i = 0; ok && i < overview; i += chunk) {
    const size_t count = (size_t)Min((uint64_t)chunk, overview - i);
    ok = fwrite(tile.weights, sizeof(uint32_t), count, file) == count;
  }
  ok = fflush(file) == 0 && ok;
  ok = fclose(file) == 0 && ok;
  return heightfield_commit(temp, path, ok, error, capacity);
}

static bool8_t heightfield_write_overview(const VkrHeightfield *field,
                                          FILE *file) {
  const uint64_t overview = heightfield_overview_count(field->cells);
  return fwrite(field->overview_heights, sizeof(uint16_t), overview, file) ==
             overview &&
         fwrite(field->overview_weights, sizeof(uint32_t), overview, file) ==
             overview;
}

/* Writes resident tile `index` of a streamed field at its offset. */
static bool8_t heightfield_store_tile(VkrHeightfield *field, uint32_t index) {
  FILE *file = field->file;
  if (FSEEK64(file, (int64_t)heightfield_tile_offset(index), SEEK_SET) != 0 ||
      fwrite(field->tiles[index], sizeof(VkrHeightfieldTile), 1u, file) != 1u) {
    return false_v;
  }
  field->tile_dirty[index] = 0u;
  return true_v;
}

bool8_t vkr_heightfield_save(VkrHeightfield *field, const char *path,
                             char *error, uint32_t capacity) {
  const uint32_t tiles = field->tiles_per_side * field->tiles_per_side;
  if (field->file) {
    /* A streamed field writes its changed tiles and the overview in place. */
    bool8_t ok = true_v;
    for (uint32_t i = 0; ok && i < tiles; ++i) {
      if (field->tiles[i] && field->tile_dirty[i]) {
        ok = heightfield_store_tile(field, i);
      }
    }
    ok = ok &&
         FSEEK64(field->file, (int64_t)heightfield_tile_offset(tiles),
                 SEEK_SET) == 0 &&
         heightfield_write_overview(field, field->file) &&
         fflush(field->file) == 0;
    return ok ? true_v
              : heightfield_fail(error, capacity,
                                 "The terrain file could not be written");
  }
  char temp[1100];
  if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) {
    return heightfield_fail(error, capacity, "The terrain path is too long");
  }
  FILE *file = file_fopen(temp, "wb");
  if (!file) {
    return heightfield_fail(error, capacity,
                            "The terrain file could not be created");
  }
  bool8_t ok = heightfield_write_header(file, field->cells, field->spacing,
                                        field->height_min, field->height_max);
  for (uint32_t i = 0; ok && i < tiles; ++i) {
    ok = field->tiles[i] &&
         fwrite(field->tiles[i], sizeof(VkrHeightfieldTile), 1u, file) == 1u;
  }
  ok = ok && heightfield_write_overview(field, file);
  ok = fflush(file) == 0 && ok;
  ok = fclose(file) == 0 && ok;
  if (ok) {
    MemZero(field->tile_dirty, tiles * sizeof(uint8_t));
  }
  return heightfield_commit(temp, path, ok, error, capacity);
}

bool8_t vkr_heightfield_load(VkrHeightfield *out, const char *path,
                             VkrAllocator *allocator, char *error,
                             uint32_t capacity) {
  MemZero(out, sizeof(*out));
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return heightfield_fail(error, capacity,
                            "The terrain file could not be opened");
  }
  HeightfieldHeader header;
  bool8_t ok =
      fread(&header, sizeof(header), 1u, file) == 1u &&
      MemCompare(header.magic, s_heightfield_magic, sizeof(header.magic)) ==
          0 &&
      (header.version == 1u ||
       (header.version == 2u &&
        header.overview_stride == VKR_HEIGHTFIELD_OVERVIEW_STRIDE)) &&
      header.tile_samples == VKR_HEIGHTFIELD_TILE_SAMPLES &&
      heightfield_valid(header.cells, header.spacing, header.height_min,
                        header.height_max) &&
      header.tiles_per_side == heightfield_tiles(header.cells + 1u) &&
      (header.version == 2u ||
       header.cells <= VKR_HEIGHTFIELD_RESIDENT_CELLS) &&
      heightfield_arrays(out, header.cells, header.spacing, header.height_min,
                         header.height_max, allocator);
  if (!ok) {
    fclose(file);
    vkr_heightfield_destroy(out, allocator);
    return heightfield_fail(error, capacity,
                            "The terrain file is not a supported heightfield");
  }
  const uint32_t tiles = out->tiles_per_side * out->tiles_per_side;
  const uint64_t overview = heightfield_overview_count(out->cells);
  if (out->cells <= VKR_HEIGHTFIELD_RESIDENT_CELLS) {
    for (uint32_t i = 0; ok && i < tiles; ++i) {
      VkrHeightfieldTile *tile = heightfield_tile_new(out, i);
      ok = tile && fread(tile, sizeof(*tile), 1u, file) == 1u;
    }
    /* Version 1 has no overview; the samples give it. */
    heightfield_overview_refresh(
        out, (VkrHeightfieldRect){0u, 0u, out->cells, out->cells});
    fclose(file);
  } else {
    /* A streamed field reads its overview now and its tiles on demand. */
    ok =
        FSEEK64(file, (int64_t)heightfield_tile_offset(tiles), SEEK_SET) == 0 &&
        fread(out->overview_heights, sizeof(uint16_t), overview, file) ==
            overview &&
        fread(out->overview_weights, sizeof(uint32_t), overview, file) ==
            overview;
    fclose(file);
    out->file = ok ? file_fopen(path, "r+b") : NULL;
    ok = ok && out->file;
  }
  if (!ok) {
    vkr_heightfield_destroy(out, allocator);
    return heightfield_fail(error, capacity, "The terrain file is truncated");
  }
  return true_v;
}

bool8_t vkr_heightfield_tile_load(VkrHeightfield *field, uint32_t tx,
                                  uint32_t tz) {
  const uint32_t index = tz * field->tiles_per_side + tx;
  if (field->tiles[index]) {
    return true_v;
  }
  if (!field->file) {
    return false_v;
  }
  VkrHeightfieldTile *tile = heightfield_tile_new(field, index);
  if (!tile ||
      FSEEK64(field->file, (int64_t)heightfield_tile_offset(index), SEEK_SET) !=
          0 ||
      fread(tile, sizeof(*tile), 1u, field->file) != 1u) {
    if (tile) {
      heightfield_tile_free(field, index);
    }
    return false_v;
  }
  return true_v;
}

bool8_t vkr_heightfield_tile_release(VkrHeightfield *field, uint32_t tx,
                                     uint32_t tz) {
  const uint32_t index = tz * field->tiles_per_side + tx;
  /* Unsaved samples stay until a save writes them. */
  if (!field->tiles[index] || !field->file || field->tile_dirty[index]) {
    return false_v;
  }
  heightfield_tile_free(field, index);
  return true_v;
}

bool8_t vkr_heightfield_load_rect(VkrHeightfield *field,
                                  VkrHeightfieldRect rect) {
  const uint32_t size = VKR_HEIGHTFIELD_TILE_SAMPLES;
  for (uint32_t tz = rect.z0 / size; tz <= rect.z1 / size; ++tz) {
    for (uint32_t tx = rect.x0 / size; tx <= rect.x1 / size; ++tx) {
      if (!vkr_heightfield_tile_load(field, tx, tz)) {
        return false_v;
      }
    }
  }
  return true_v;
}
