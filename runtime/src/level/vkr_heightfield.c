#include "level/vkr_heightfield.h"

#include "filesystem/filesystem.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define HEIGHTFIELD_TAG VKR_ALLOCATOR_MEMORY_TAG_ARRAY
#define HEIGHTFIELD_VERSION 1u

/* The file header; samples follow in tiles. */
typedef struct HeightfieldHeader {
  char magic[8];
  uint32_t version;
  uint32_t cells;
  float32_t spacing;
  float32_t height_min;
  float32_t height_max;
  uint32_t tile_samples;
  uint32_t tiles_per_side;
  uint32_t reserved[3];
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

bool8_t vkr_heightfield_create(VkrHeightfield *out, uint32_t cells,
                               float32_t spacing, float32_t height_min,
                               float32_t height_max, float32_t height,
                               VkrAllocator *allocator) {
  MemZero(out, sizeof(*out));
  if (!cells || cells % VKR_HEIGHTFIELD_TILE_CELLS ||
      cells > VKR_HEIGHTFIELD_CELLS_MAX || !(spacing > 0.0f) ||
      !isfinite(spacing) || !isfinite(height_min) || !isfinite(height_max) ||
      !(height_max > height_min)) {
    return false_v;
  }
  const uint64_t count = (uint64_t)(cells + 1u) * (cells + 1u);
  out->heights =
      vkr_allocator_alloc(allocator, count * sizeof(uint16_t), HEIGHTFIELD_TAG);
  out->weights =
      vkr_allocator_alloc(allocator, count * sizeof(uint32_t), HEIGHTFIELD_TAG);
  if (!out->heights || !out->weights) {
    vkr_heightfield_destroy(out, allocator);
    return false_v;
  }
  out->cells = cells;
  out->spacing = spacing;
  out->height_min = height_min;
  out->height_max = height_max;
  const uint16_t value = vkr_heightfield_quantize(out, height);
  for (uint64_t i = 0; i < count; ++i) {
    out->heights[i] = value;
    out->weights[i] = 0xFFu;
  }
  return true_v;
}

void vkr_heightfield_destroy(VkrHeightfield *field, VkrAllocator *allocator) {
  const uint64_t count =
      (uint64_t)(field->cells + 1u) * (uint64_t)(field->cells + 1u);
  if (field->heights) {
    vkr_allocator_free(allocator, field->heights, count * sizeof(uint16_t),
                       HEIGHTFIELD_TAG);
  }
  if (field->weights) {
    vkr_allocator_free(allocator, field->weights, count * sizeof(uint32_t),
                       HEIGHTFIELD_TAG);
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
  return vkr_heightfield_metres(
      field, field->heights[(size_t)z * vkr_heightfield_samples(field) + x]);
}

bool8_t vkr_heightfield_sample(const VkrHeightfield *field, float32_t x,
                               float32_t z, float32_t *out_height) {
  const float32_t half = vkr_heightfield_half_size(field);
  const float32_t gx = (x + half) / field->spacing;
  const float32_t gz = (z + half) / field->spacing;
  if (!(gx >= 0.0f) || !(gz >= 0.0f) || gx > (float32_t)field->cells ||
      gz > (float32_t)field->cells) {
    return false_v;
  }
  const uint32_t x0 = Min((uint32_t)gx, field->cells - 1u);
  const uint32_t z0 = Min((uint32_t)gz, field->cells - 1u);
  const float32_t fx = gx - (float32_t)x0;
  const float32_t fz = gz - (float32_t)z0;
  const float32_t h00 = vkr_heightfield_at(field, x0, z0);
  const float32_t h10 = vkr_heightfield_at(field, x0 + 1u, z0);
  const float32_t h01 = vkr_heightfield_at(field, x0, z0 + 1u);
  const float32_t h11 = vkr_heightfield_at(field, x0 + 1u, z0 + 1u);
  *out_height = (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fz) +
                (h01 * (1.0f - fx) + h11 * fx) * fz;
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
  const uint32_t samples = vkr_heightfield_samples(field);
  const uint32_t width = rect.x1 - rect.x0 + 1u;
  for (uint32_t z = rect.z0; z <= rect.z1; ++z) {
    const size_t from = (size_t)z * samples + rect.x0;
    const size_t to = (size_t)(z - rect.z0) * width;
    MemCopy(heights + to, field->heights + from, width * sizeof(uint16_t));
    MemCopy(weights + to, field->weights + from, width * sizeof(uint32_t));
  }
}

void vkr_heightfield_write_rect(VkrHeightfield *field, VkrHeightfieldRect rect,
                                const uint16_t *heights,
                                const uint32_t *weights) {
  const uint32_t samples = vkr_heightfield_samples(field);
  const uint32_t width = rect.x1 - rect.x0 + 1u;
  for (uint32_t z = rect.z0; z <= rect.z1; ++z) {
    const size_t to = (size_t)z * samples + rect.x0;
    const size_t from = (size_t)(z - rect.z0) * width;
    MemCopy(field->heights + to, heights + from, width * sizeof(uint16_t));
    MemCopy(field->weights + to, weights + from, width * sizeof(uint32_t));
  }
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
  const size_t index = (size_t)z * vkr_heightfield_samples(field) + x;
  const float32_t now = vkr_heightfield_metres(field, field->heights[index]);
  field->heights[index] =
      vkr_heightfield_quantize(field, now + (target - now) * t);
}

/* Adds `t` of layer `layer` to sample `index`, keeping weights summing to
   255. */
static void heightfield_paint(VkrHeightfield *field, size_t index,
                              uint32_t layer, float32_t t) {
  float32_t w[VKR_HEIGHTFIELD_LAYERS];
  float32_t sum = 0.0f;
  for (uint32_t i = 0; i < VKR_HEIGHTFIELD_LAYERS; ++i) {
    w[i] = (float32_t)((field->weights[index] >> (8u * i)) & 0xFFu);
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
  field->weights[index] =
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
      const size_t index = (size_t)sz * samples + sx;
      const float32_t now =
          vkr_heightfield_metres(field, field->heights[index]);
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
        heightfield_paint(field, index, layer,
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
          field->heights[(size_t)sz * samples + sx] =
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
  default:
    return false_v;
  }
}

bool8_t vkr_heightfield_op_apply(VkrHeightfield *field,
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
  default:
    return false_v;
  }
}

// =============================================================================
// Files
// =============================================================================

static uint32_t heightfield_tiles(uint32_t samples) {
  return (samples + VKR_HEIGHTFIELD_TILE_SAMPLES - 1u) /
         VKR_HEIGHTFIELD_TILE_SAMPLES;
}

bool8_t vkr_heightfield_save(const VkrHeightfield *field, const char *path,
                             char *error, uint32_t capacity) {
  const uint32_t samples = vkr_heightfield_samples(field);
  const uint32_t tiles = heightfield_tiles(samples);
  const uint32_t tile_samples =
      VKR_HEIGHTFIELD_TILE_SAMPLES * VKR_HEIGHTFIELD_TILE_SAMPLES;
  char temp[1100];
  if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) {
    return heightfield_fail(error, capacity, "The terrain path is too long");
  }
  FILE *file = file_fopen(temp, "wb");
  if (!file) {
    return heightfield_fail(error, capacity,
                            "The terrain file could not be created");
  }
  HeightfieldHeader header = {.version = HEIGHTFIELD_VERSION,
                              .cells = field->cells,
                              .spacing = field->spacing,
                              .height_min = field->height_min,
                              .height_max = field->height_max,
                              .tile_samples = VKR_HEIGHTFIELD_TILE_SAMPLES,
                              .tiles_per_side = tiles};
  MemCopy(header.magic, s_heightfield_magic, sizeof(header.magic));
  bool8_t ok = fwrite(&header, sizeof(header), 1u, file) == 1u;
  uint16_t heights[VKR_HEIGHTFIELD_TILE_SAMPLES * VKR_HEIGHTFIELD_TILE_SAMPLES];
  uint32_t weights[VKR_HEIGHTFIELD_TILE_SAMPLES * VKR_HEIGHTFIELD_TILE_SAMPLES];
  for (uint32_t tz = 0; ok && tz < tiles; ++tz) {
    for (uint32_t tx = 0; ok && tx < tiles; ++tx) {
      /* Samples past the field's edge in the last tiles are zero. */
      MemZero(heights, sizeof(heights));
      MemZero(weights, sizeof(weights));
      for (uint32_t z = 0; z < VKR_HEIGHTFIELD_TILE_SAMPLES; ++z) {
        const uint32_t sz = tz * VKR_HEIGHTFIELD_TILE_SAMPLES + z;
        for (uint32_t x = 0; sz < samples && x < VKR_HEIGHTFIELD_TILE_SAMPLES;
             ++x) {
          const uint32_t sx = tx * VKR_HEIGHTFIELD_TILE_SAMPLES + x;
          if (sx < samples) {
            heights[z * VKR_HEIGHTFIELD_TILE_SAMPLES + x] =
                field->heights[(size_t)sz * samples + sx];
            weights[z * VKR_HEIGHTFIELD_TILE_SAMPLES + x] =
                field->weights[(size_t)sz * samples + sx];
          }
        }
      }
      ok =
          fwrite(heights, sizeof(uint16_t), tile_samples, file) ==
              tile_samples &&
          fwrite(weights, sizeof(uint32_t), tile_samples, file) == tile_samples;
    }
  }
  ok = fflush(file) == 0 && ok;
  ok = fclose(file) == 0 && ok;
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
  bool8_t ok = fread(&header, sizeof(header), 1u, file) == 1u &&
               MemCompare(header.magic, s_heightfield_magic,
                          sizeof(header.magic)) == 0 &&
               header.version == HEIGHTFIELD_VERSION &&
               header.tile_samples == VKR_HEIGHTFIELD_TILE_SAMPLES &&
               header.tiles_per_side == heightfield_tiles(header.cells + 1u) &&
               vkr_heightfield_create(out, header.cells, header.spacing,
                                      header.height_min, header.height_max,
                                      header.height_min, allocator);
  if (!ok) {
    fclose(file);
    vkr_heightfield_destroy(out, allocator);
    return heightfield_fail(error, capacity,
                            "The terrain file is not a supported heightfield");
  }
  const uint32_t samples = vkr_heightfield_samples(out);
  const uint32_t tile_samples =
      VKR_HEIGHTFIELD_TILE_SAMPLES * VKR_HEIGHTFIELD_TILE_SAMPLES;
  uint16_t heights[VKR_HEIGHTFIELD_TILE_SAMPLES * VKR_HEIGHTFIELD_TILE_SAMPLES];
  uint32_t weights[VKR_HEIGHTFIELD_TILE_SAMPLES * VKR_HEIGHTFIELD_TILE_SAMPLES];
  for (uint32_t tz = 0; ok && tz < header.tiles_per_side; ++tz) {
    for (uint32_t tx = 0; ok && tx < header.tiles_per_side; ++tx) {
      ok = fread(heights, sizeof(uint16_t), tile_samples, file) ==
               tile_samples &&
           fread(weights, sizeof(uint32_t), tile_samples, file) == tile_samples;
      for (uint32_t z = 0; ok && z < VKR_HEIGHTFIELD_TILE_SAMPLES; ++z) {
        const uint32_t sz = tz * VKR_HEIGHTFIELD_TILE_SAMPLES + z;
        for (uint32_t x = 0; sz < samples && x < VKR_HEIGHTFIELD_TILE_SAMPLES;
             ++x) {
          const uint32_t sx = tx * VKR_HEIGHTFIELD_TILE_SAMPLES + x;
          if (sx < samples) {
            out->heights[(size_t)sz * samples + sx] =
                heights[z * VKR_HEIGHTFIELD_TILE_SAMPLES + x];
            out->weights[(size_t)sz * samples + sx] =
                weights[z * VKR_HEIGHTFIELD_TILE_SAMPLES + x];
          }
        }
      }
    }
  }
  fclose(file);
  if (!ok) {
    vkr_heightfield_destroy(out, allocator);
    return heightfield_fail(error, capacity, "The terrain file is truncated");
  }
  return true_v;
}
