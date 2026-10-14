#include "assets/vkr_diffuse_volume.h"

#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(VKR_CHAR_BITS == 8, "DVOL requires 8-bit bytes");
_Static_assert(sizeof(float32_t) == 4u, "DVOL requires 32-bit float32_t");
_Static_assert(VKR_DIFFUSE_VOLUME_MOMENT_BYTES ==
                   VKR_DIFFUSE_VOLUME_MOMENT_TEXELS * 2u * sizeof(uint16_t),
               "DVOL moment tile drift");
_Static_assert(VKR_DIFFUSE_VOLUME_MOMENT_TEXELS ==
                   VKR_DIFFUSE_VOLUME_MOMENT_SIZE *
                       VKR_DIFFUSE_VOLUME_MOMENT_SIZE,
               "DVOL moment tile drift");
_Static_assert(VKR_DIFFUSE_VOLUME_MAX_BRICKS *(uint64_t)
                       VKR_DIFFUSE_VOLUME_BRICK_PROBES <=
                   VKR_DIFFUSE_VOLUME_ENTRY_BRICK_MASK,
               "DVOL brick index range");

#define VKR_DIFFUSE_VOLUME_H_MAGIC 0u
#define VKR_DIFFUSE_VOLUME_H_VERSION 4u
#define VKR_DIFFUSE_VOLUME_H_ENDIAN 8u
#define VKR_DIFFUSE_VOLUME_H_SIZE 12u
#define VKR_DIFFUSE_VOLUME_H_FILE_SIZE 16u
#define VKR_DIFFUSE_VOLUME_H_ORIGIN 24u
#define VKR_DIFFUSE_VOLUME_H_SPACING 36u
#define VKR_DIFFUSE_VOLUME_H_LEVEL_COUNT 40u
#define VKR_DIFFUSE_VOLUME_H_DIMENSIONS 44u
#define VKR_DIFFUSE_VOLUME_H_BRICK_COUNT 56u
#define VKR_DIFFUSE_VOLUME_H_LAYER_COUNT 60u
#define VKR_DIFFUSE_VOLUME_H_ENTRY_OFFSET 64u
#define VKR_DIFFUSE_VOLUME_H_BRICK_OFFSET 72u
#define VKR_DIFFUSE_VOLUME_H_AUX_OFFSET 80u
#define VKR_DIFFUSE_VOLUME_H_MOMENT_OFFSET 88u
#define VKR_DIFFUSE_VOLUME_H_SH_OFFSET 96u
#define VKR_DIFFUSE_VOLUME_H_PAYLOAD_CRC 104u
#define VKR_DIFFUSE_VOLUME_H_HEADER_CRC 108u
#define VKR_DIFFUSE_VOLUME_H_SH_SCALE 112u
#define VKR_DIFFUSE_VOLUME_H_LAMP_DIRECT_COUNT 116u
#define VKR_DIFFUSE_VOLUME_H_RESERVED 120u

#define VKR_DIFFUSE_VOLUME_BRICK_RECORD_BYTES 16u

/* Section offsets and the file size that follow from a volume's counts. */
typedef struct VkrDiffuseVolumeSections {
  uint64_t entry_offset;
  uint64_t brick_offset;
  uint64_t aux_offset;
  uint64_t moment_offset;
  uint64_t sh_offset;
  uint64_t file_size;
} VkrDiffuseVolumeSections;

static uint32_t vkr_diffuse_volume_header_crc(const uint8_t *header) {
  uint32_t crc = vkr_crc32_update(VKR_CRC32_INITIAL, header,
                                  VKR_DIFFUSE_VOLUME_H_HEADER_CRC);
  const uint8_t zero_crc[4] = {0u, 0u, 0u, 0u};
  crc = vkr_crc32_update(crc, zero_crc, sizeof(zero_crc));
  crc = vkr_crc32_update(
      crc, header + VKR_DIFFUSE_VOLUME_H_HEADER_CRC + sizeof(zero_crc),
      VKR_DIFFUSE_VOLUME_HEADER_BYTES -
          (VKR_DIFFUSE_VOLUME_H_HEADER_CRC + sizeof(zero_crc)));
  return ~crc;
}

static bool8_t vkr_diffuse_volume_finite_vec3(Vec3 value) {
  return vkr_is_finite_f32(value.x) && vkr_is_finite_f32(value.y) &&
         vkr_is_finite_f32(value.z);
}

static uint32_t vkr_diffuse_volume_level_span(uint32_t level) {
  uint32_t span = 1u;
  for (uint32_t i = 0u; i < level; ++i) {
    span *= 3u;
  }
  return span;
}

static uint16_t vkr_diffuse_volume_load_half(const uint8_t *src) {
  return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static bool8_t vkr_diffuse_volume_half_finite(uint16_t half) {
  return (half & 0x7c00u) != 0x7c00u;
}

/* Every count is in range and the grid's dimensions are whole top-level
   blocks. */
static bool8_t vkr_diffuse_volume_counts_valid(const uint32_t dimensions[3],
                                               uint32_t level_count,
                                               uint32_t brick_count,
                                               uint32_t layer_count,
                                               uint32_t *out_entry_count) {
  if (level_count == 0u || level_count > VKR_DIFFUSE_VOLUME_MAX_LEVELS ||
      brick_count == 0u || brick_count > VKR_DIFFUSE_VOLUME_MAX_BRICKS ||
      layer_count == 0u || layer_count > VKR_DIFFUSE_VOLUME_MAX_LAYERS) {
    return false_v;
  }
  const uint32_t block = vkr_diffuse_volume_level_span(level_count - 1u);
  uint64_t entry_count = 1u;
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    if (dimensions[axis] == 0u || dimensions[axis] % block != 0u ||
        !vkr_checked_mul_u64(entry_count, dimensions[axis], &entry_count) ||
        entry_count > VKR_DIFFUSE_VOLUME_MAX_ENTRIES) {
      return false_v;
    }
  }
  *out_entry_count = (uint32_t)entry_count;
  return true_v;
}

static bool8_t vkr_diffuse_volume_sections(uint32_t entry_count,
                                           uint32_t brick_count,
                                           uint32_t layer_count,
                                           uint32_t band_count,
                                           VkrDiffuseVolumeSections *out) {
  const uint64_t alignment = VKR_DIFFUSE_VOLUME_SECTION_ALIGNMENT;
  const uint64_t probe_count =
      (uint64_t)brick_count * VKR_DIFFUSE_VOLUME_BRICK_PROBES;
  uint64_t cursor = VKR_DIFFUSE_VOLUME_HEADER_BYTES +
                    (uint64_t)layer_count * VKR_LIGHT_LAYER_RECORD_BYTES;
  out->entry_offset = vkr_align_up_u64(cursor, alignment);
  cursor = out->entry_offset + (uint64_t)entry_count * sizeof(uint32_t);
  out->brick_offset = vkr_align_up_u64(cursor, alignment);
  cursor = out->brick_offset +
           (uint64_t)brick_count * VKR_DIFFUSE_VOLUME_BRICK_RECORD_BYTES;
  out->aux_offset = vkr_align_up_u64(cursor, alignment);
  cursor = out->aux_offset + probe_count * VKR_DIFFUSE_VOLUME_AUX_BYTES;
  out->moment_offset = vkr_align_up_u64(cursor, alignment);
  cursor = out->moment_offset + probe_count * VKR_DIFFUSE_VOLUME_MOMENT_BYTES;
  out->sh_offset = vkr_align_up_u64(cursor, alignment);
  uint64_t sh_bytes = 0u;
  if (!vkr_checked_mul_u64(probe_count * band_count,
                           VKR_DIFFUSE_VOLUME_SH_BYTES, &sh_bytes) ||
      !vkr_checked_add_u64(out->sh_offset, sh_bytes, &out->file_size)) {
    return false_v;
  }
  return true_v;
}

uint32_t vkr_diffuse_volume_sh_band_count(const VkrDiffuseVolume *volume) {
  return volume->layer_count + volume->lamp_direct_count;
}

/* Zero lamp direct bands, or one per lamp-group layer. */
static bool8_t vkr_diffuse_volume_direct_valid(const VkrDiffuseVolume *volume) {
  if (volume->lamp_direct_count == 0u) {
    return true_v;
  }
  uint32_t lamp_groups = 0u;
  for (uint32_t layer = 0u; layer < volume->layer_count; ++layer) {
    if (volume->layers[layer].kind == VKR_LIGHT_LAYER_LAMP_GROUP) {
      ++lamp_groups;
    }
  }
  return volume->lamp_direct_count == lamp_groups;
}

uint32_t vkr_diffuse_volume_entry(const VkrDiffuseVolume *volume,
                                  uint32_t index) {
  return vkr_load_le_u32(volume->entries + (uint64_t)index * sizeof(uint32_t));
}

/* Rec. 709 luminance of the constant term of one probe's band; each of the
   red, green and blue rows holds linear x, y, z, then the constant. */
static float32_t
vkr_diffuse_volume_band_luminance(const VkrDiffuseVolume *volume, uint32_t band,
                                  uint32_t probe) {
  const uint8_t *rows =
      volume->layer_sh + ((uint64_t)band * volume->probe_count + probe) *
                             VKR_DIFFUSE_VOLUME_SH_BYTES;
  const uint32_t row_bytes = VKR_DIFFUSE_VOLUME_SH_BYTES / 3u;
  const uint32_t constant = 3u * sizeof(uint16_t);
  const float32_t red =
      vkr_float16_to_float32(vkr_diffuse_volume_load_half(rows + constant));
  const float32_t green = vkr_float16_to_float32(
      vkr_diffuse_volume_load_half(rows + row_bytes + constant));
  const float32_t blue = vkr_float16_to_float32(
      vkr_diffuse_volume_load_half(rows + 2u * row_bytes + constant));
  return 0.2126f * red + 0.7152f * green + 0.0722f * blue;
}

bool8_t vkr_diffuse_volume_layer_luminance(
    const VkrDiffuseVolume *volume, Vec3 position,
    float32_t out_luminance[VKR_DIFFUSE_VOLUME_MAX_LAYERS]) {
  if (!volume || !out_luminance || !vkr_diffuse_volume_finite_vec3(position) ||
      !(volume->spacing > 0.0f)) {
    return false_v;
  }

  /* The indirection entry, as vkr_diffuse_volume_entry() in the shaders. */
  const float32_t entry_size = 3.0f * volume->spacing;
  uint32_t entry[3];
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t cell = vkr_floor_f32(
        (position.elements[axis] - volume->origin.elements[axis]) / entry_size);
    if (!(cell >= 0.0f) || cell >= (float32_t)volume->dimensions[axis]) {
      return false_v;
    }
    entry[axis] = (uint32_t)cell;
  }
  const uint32_t value = vkr_diffuse_volume_entry(
      volume, entry[0] + volume->dimensions[0] *
                             (entry[1] + volume->dimensions[1] * entry[2]));
  if (value == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY) {
    return false_v;
  }

  /* The brick cell, as vkr_diffuse_volume_brick_cell(). */
  const uint32_t span = vkr_diffuse_volume_level_span(
      value >> VKR_DIFFUSE_VOLUME_ENTRY_LEVEL_SHIFT);
  const uint32_t first_probe = (value & VKR_DIFFUSE_VOLUME_ENTRY_BRICK_MASK) *
                               VKR_DIFFUSE_VOLUME_BRICK_PROBES;
  const float32_t step = volume->spacing * (float32_t)span;
  uint32_t base[3];
  float32_t fraction[3];
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    const float32_t brick_origin =
        volume->origin.elements[axis] +
        entry_size * (float32_t)(entry[axis] - entry[axis] % span);
    const float32_t offset = (position.elements[axis] - brick_origin) / step;
    const float32_t local = Clamp(offset, 0.0f, 3.0f);
    base[axis] = Min((uint32_t)vkr_floor_f32(local), 2u);
    fraction[axis] = local - (float32_t)base[axis];
  }

  MemZero(out_luminance, sizeof(float32_t) * VKR_DIFFUSE_VOLUME_MAX_LAYERS);
  float32_t total = 0.0f;
  for (uint32_t corner = 0u; corner < 8u; ++corner) {
    const uint32_t c[3] = {corner & 1u, (corner >> 1u) & 1u,
                           (corner >> 2u) & 1u};
    const uint32_t probe =
        first_probe + (base[0] + c[0]) +
        VKR_DIFFUSE_VOLUME_BRICK_SIZE *
            ((base[1] + c[1]) +
             VKR_DIFFUSE_VOLUME_BRICK_SIZE * (base[2] + c[2]));
    const uint8_t *aux =
        volume->probe_aux + (uint64_t)probe * VKR_DIFFUSE_VOLUME_AUX_BYTES;
    float32_t weight =
        vkr_float16_to_float32(vkr_diffuse_volume_load_half(aux + 6u));
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      weight *= c[axis] ? fraction[axis] : 1.0f - fraction[axis];
    }
    if (!(weight > 0.0f)) {
      continue;
    }
    total += weight;

    uint32_t lamp_group = 0u;
    for (uint32_t layer = 0u; layer < volume->layer_count; ++layer) {
      float32_t luminance =
          vkr_diffuse_volume_band_luminance(volume, layer, probe);
      if (volume->layers[layer].kind == VKR_LIGHT_LAYER_LAMP_GROUP) {
        if (volume->lamp_direct_count != 0u) {
          luminance += vkr_diffuse_volume_band_luminance(
              volume, volume->layer_count + lamp_group, probe);
        }
        ++lamp_group;
      }
      out_luminance[layer] += weight * luminance;
    }
  }
  if (!(total > 0.0f)) {
    return false_v;
  }

  for (uint32_t layer = 0u; layer < volume->layer_count; ++layer) {
    out_luminance[layer] =
        Max(out_luminance[layer] / total, 0.0f) * volume->sh_scale;
  }
  return true_v;
}

static bool8_t vkr_diffuse_volume_bricks_valid(const VkrDiffuseVolume *volume) {
  for (uint32_t b = 0u; b < volume->brick_count; ++b) {
    const VkrDiffuseVolumeBrick *brick = &volume->bricks[b];
    if (brick->level >= volume->level_count) {
      return false_v;
    }
    const uint32_t span = vkr_diffuse_volume_level_span(brick->level);
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      if (brick->entry[axis] % span != 0u ||
          brick->entry[axis] >= volume->dimensions[axis] ||
          volume->dimensions[axis] - brick->entry[axis] < span) {
        return false_v;
      }
    }
  }
  return true_v;
}

/* Each entry is empty or names a brick of its level that covers it. */
static bool8_t
vkr_diffuse_volume_entries_valid(const VkrDiffuseVolume *volume) {
  uint32_t index = 0u;
  for (uint32_t z = 0u; z < volume->dimensions[2]; ++z) {
    for (uint32_t y = 0u; y < volume->dimensions[1]; ++y) {
      for (uint32_t x = 0u; x < volume->dimensions[0]; ++x) {
        const uint32_t entry = vkr_diffuse_volume_entry(volume, index);
        ++index;
        if (entry == VKR_DIFFUSE_VOLUME_ENTRY_EMPTY) {
          continue;
        }
        const uint32_t level = entry >> VKR_DIFFUSE_VOLUME_ENTRY_LEVEL_SHIFT;
        const uint32_t brick = entry & VKR_DIFFUSE_VOLUME_ENTRY_BRICK_MASK;
        if (brick >= volume->brick_count ||
            volume->bricks[brick].level != level) {
          return false_v;
        }
        const uint32_t span = vkr_diffuse_volume_level_span(level);
        const uint32_t coordinate[3] = {x, y, z};
        for (uint32_t axis = 0u; axis < 3u; ++axis) {
          const uint32_t first = volume->bricks[brick].entry[axis];
          if (coordinate[axis] < first || coordinate[axis] - first >= span) {
            return false_v;
          }
        }
      }
    }
  }
  return true_v;
}

/* Offsets stay within half their level's spacing (with half-precision
   rounding), validity is exactly zero or one, and every half is finite. */
static bool8_t vkr_diffuse_volume_probes_valid(const VkrDiffuseVolume *volume) {
  for (uint32_t p = 0u; p < volume->probe_count; ++p) {
    const uint32_t level =
        volume->bricks[p / VKR_DIFFUSE_VOLUME_BRICK_PROBES].level;
    const float32_t limit = 0.5f * volume->spacing *
                            (float32_t)vkr_diffuse_volume_level_span(level) *
                            1.001f;
    const uint8_t *aux =
        volume->probe_aux + (uint64_t)p * VKR_DIFFUSE_VOLUME_AUX_BYTES;
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      const uint16_t half = vkr_diffuse_volume_load_half(aux + axis * 2u);
      if (!vkr_diffuse_volume_half_finite(half) ||
          vkr_abs_f32(vkr_float16_to_float32(half)) > limit) {
        return false_v;
      }
    }
    const uint16_t validity = vkr_diffuse_volume_load_half(aux + 6u);
    if (validity != 0x0000u && validity != 0x3c00u) {
      return false_v;
    }
  }
  const uint64_t moment_halves =
      (uint64_t)volume->probe_count * VKR_DIFFUSE_VOLUME_MOMENT_TEXELS * 2u;
  for (uint64_t i = 0u; i < moment_halves; ++i) {
    const uint16_t half =
        vkr_diffuse_volume_load_half(volume->moments + i * 2u);
    if (!vkr_diffuse_volume_half_finite(half) || (half & 0x8000u) != 0u) {
      return false_v;
    }
  }
  const uint64_t sh_halves = (uint64_t)volume->probe_count *
                             vkr_diffuse_volume_sh_band_count(volume) *
                             (VKR_DIFFUSE_VOLUME_SH_BYTES / 2u);
  for (uint64_t i = 0u; i < sh_halves; ++i) {
    if (!vkr_diffuse_volume_half_finite(
            vkr_diffuse_volume_load_half(volume->layer_sh + i * 2u))) {
      return false_v;
    }
  }
  return true_v;
}

/* A positive, finite power of two, so scaling halves loses no precision. */
static bool8_t vkr_diffuse_volume_scale_valid(float32_t scale) {
  int32_t exponent = 0;
  return vkr_is_finite_f32(scale) && scale > 0.0f &&
         vkr_frexp_f32(scale, &exponent) == 0.5f;
}

static bool8_t vkr_diffuse_volume_valid(const VkrDiffuseVolume *volume) {
  uint32_t entry_count = 0u;
  if (!volume || !volume->entries || !volume->bricks || !volume->probe_aux ||
      !volume->moments || !volume->layer_sh || !volume->layers ||
      !vkr_diffuse_volume_counts_valid(volume->dimensions, volume->level_count,
                                       volume->brick_count, volume->layer_count,
                                       &entry_count) ||
      volume->entry_count != entry_count ||
      volume->probe_count !=
          volume->brick_count * VKR_DIFFUSE_VOLUME_BRICK_PROBES ||
      !vkr_light_layers_valid(volume->layers, volume->layer_count) ||
      !vkr_diffuse_volume_direct_valid(volume) ||
      !vkr_diffuse_volume_finite_vec3(volume->origin) ||
      !vkr_is_finite_f32(volume->spacing) || !(volume->spacing > 0.0f) ||
      !vkr_diffuse_volume_scale_valid(volume->sh_scale)) {
    return false_v;
  }
  return vkr_diffuse_volume_bricks_valid(volume) &&
         vkr_diffuse_volume_entries_valid(volume) &&
         vkr_diffuse_volume_probes_valid(volume);
}

bool8_t vkr_diffuse_volume_encode(const VkrDiffuseVolume *volume, Arena *arena,
                                  const uint8_t **out_bytes,
                                  uint64_t *out_size) {
  if (!out_bytes || !out_size) {
    return false_v;
  }
  *out_bytes = NULL;
  *out_size = 0u;
  VkrDiffuseVolumeSections sections = {0};
  if (!arena || !vkr_f32_is_binary32() || !vkr_diffuse_volume_valid(volume) ||
      !vkr_diffuse_volume_sections(
          volume->entry_count, volume->brick_count, volume->layer_count,
          vkr_diffuse_volume_sh_band_count(volume), &sections)) {
    return false_v;
  }

  uint8_t *bytes =
      (uint8_t *)arena_alloc(arena, sections.file_size, ARENA_MEMORY_TAG_FILE);
  if (!bytes) {
    return false_v;
  }
  MemZero(bytes, sections.file_size);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_MAGIC,
                   VKR_DIFFUSE_VOLUME_MAGIC);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_VERSION,
                   VKR_DIFFUSE_VOLUME_VERSION);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_ENDIAN,
                   VKR_DIFFUSE_VOLUME_ENDIAN_TAG);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_SIZE,
                   VKR_DIFFUSE_VOLUME_HEADER_BYTES);
  vkr_store_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_FILE_SIZE, sections.file_size);
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    vkr_store_le_f32(bytes + VKR_DIFFUSE_VOLUME_H_ORIGIN +
                         axis * sizeof(float32_t),
                     volume->origin.elements[axis]);
    vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_DIMENSIONS +
                         axis * sizeof(uint32_t),
                     volume->dimensions[axis]);
  }
  vkr_store_le_f32(bytes + VKR_DIFFUSE_VOLUME_H_SPACING, volume->spacing);
  vkr_store_le_f32(bytes + VKR_DIFFUSE_VOLUME_H_SH_SCALE, volume->sh_scale);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_LEVEL_COUNT,
                   volume->level_count);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_BRICK_COUNT,
                   volume->brick_count);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_LAYER_COUNT,
                   volume->layer_count);
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_LAMP_DIRECT_COUNT,
                   volume->lamp_direct_count);
  vkr_store_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_ENTRY_OFFSET,
                   sections.entry_offset);
  vkr_store_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_BRICK_OFFSET,
                   sections.brick_offset);
  vkr_store_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_AUX_OFFSET,
                   sections.aux_offset);
  vkr_store_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_MOMENT_OFFSET,
                   sections.moment_offset);
  vkr_store_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_SH_OFFSET, sections.sh_offset);

  for (uint32_t layer = 0u; layer < volume->layer_count; ++layer) {
    vkr_light_layer_write(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES +
                              (uint64_t)layer * VKR_LIGHT_LAYER_RECORD_BYTES,
                          &volume->layers[layer]);
  }
  MemCopy(bytes + sections.entry_offset, volume->entries,
          (uint64_t)volume->entry_count * sizeof(uint32_t));
  for (uint32_t b = 0u; b < volume->brick_count; ++b) {
    uint8_t *record = bytes + sections.brick_offset +
                      (uint64_t)b * VKR_DIFFUSE_VOLUME_BRICK_RECORD_BYTES;
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      vkr_store_le_u32(record + axis * sizeof(uint32_t),
                       volume->bricks[b].entry[axis]);
    }
    vkr_store_le_u32(record + 12u, volume->bricks[b].level);
  }
  MemCopy(bytes + sections.aux_offset, volume->probe_aux,
          (uint64_t)volume->probe_count * VKR_DIFFUSE_VOLUME_AUX_BYTES);
  MemCopy(bytes + sections.moment_offset, volume->moments,
          (uint64_t)volume->probe_count * VKR_DIFFUSE_VOLUME_MOMENT_BYTES);
  MemCopy(bytes + sections.sh_offset, volume->layer_sh,
          (uint64_t)volume->probe_count *
              vkr_diffuse_volume_sh_band_count(volume) *
              VKR_DIFFUSE_VOLUME_SH_BYTES);

  vkr_store_le_u32(
      bytes + VKR_DIFFUSE_VOLUME_H_PAYLOAD_CRC,
      vkr_crc32(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES,
                sections.file_size - VKR_DIFFUSE_VOLUME_HEADER_BYTES));
  vkr_store_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_HEADER_CRC,
                   vkr_diffuse_volume_header_crc(bytes));
  *out_bytes = bytes;
  *out_size = sections.file_size;
  return true_v;
}

bool8_t vkr_diffuse_volume_decode(const uint8_t *bytes, uint64_t size,
                                  Arena *arena, VkrDiffuseVolume *out_volume) {
  if (!out_volume) {
    return false_v;
  }
  *out_volume = (VkrDiffuseVolume){0};
  if (!bytes || !arena || size < VKR_DIFFUSE_VOLUME_HEADER_BYTES ||
      !vkr_f32_is_binary32()) {
    return false_v;
  }
  if (vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_MAGIC) !=
          VKR_DIFFUSE_VOLUME_MAGIC ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_VERSION) !=
          VKR_DIFFUSE_VOLUME_VERSION ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_ENDIAN) !=
          VKR_DIFFUSE_VOLUME_ENDIAN_TAG ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_SIZE) !=
          VKR_DIFFUSE_VOLUME_HEADER_BYTES ||
      vkr_load_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_FILE_SIZE) != size ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_HEADER_CRC) !=
          vkr_diffuse_volume_header_crc(bytes)) {
    return false_v;
  }
  for (uint32_t offset = VKR_DIFFUSE_VOLUME_H_RESERVED;
       offset < VKR_DIFFUSE_VOLUME_HEADER_BYTES; offset += 4u) {
    if (vkr_load_le_u32(bytes + offset) != 0u) {
      return false_v;
    }
  }

  VkrDiffuseVolume volume = {0};
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    volume.origin.elements[axis] = vkr_load_le_f32(
        bytes + VKR_DIFFUSE_VOLUME_H_ORIGIN + axis * sizeof(float32_t));
    volume.dimensions[axis] = vkr_load_le_u32(
        bytes + VKR_DIFFUSE_VOLUME_H_DIMENSIONS + axis * sizeof(uint32_t));
  }
  volume.spacing = vkr_load_le_f32(bytes + VKR_DIFFUSE_VOLUME_H_SPACING);
  volume.sh_scale = vkr_load_le_f32(bytes + VKR_DIFFUSE_VOLUME_H_SH_SCALE);
  volume.level_count =
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_LEVEL_COUNT);
  volume.brick_count =
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_BRICK_COUNT);
  volume.layer_count =
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_LAYER_COUNT);
  volume.lamp_direct_count =
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_LAMP_DIRECT_COUNT);

  VkrDiffuseVolumeSections sections = {0};
  if (!vkr_diffuse_volume_counts_valid(volume.dimensions, volume.level_count,
                                       volume.brick_count, volume.layer_count,
                                       &volume.entry_count) ||
      volume.lamp_direct_count > VKR_LIGHT_LAYER_MAX_LAMP_GROUPS ||
      !vkr_diffuse_volume_sections(
          volume.entry_count, volume.brick_count, volume.layer_count,
          vkr_diffuse_volume_sh_band_count(&volume), &sections) ||
      sections.file_size != size ||
      vkr_load_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_ENTRY_OFFSET) !=
          sections.entry_offset ||
      vkr_load_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_BRICK_OFFSET) !=
          sections.brick_offset ||
      vkr_load_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_AUX_OFFSET) !=
          sections.aux_offset ||
      vkr_load_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_MOMENT_OFFSET) !=
          sections.moment_offset ||
      vkr_load_le_u64(bytes + VKR_DIFFUSE_VOLUME_H_SH_OFFSET) !=
          sections.sh_offset ||
      vkr_load_le_u32(bytes + VKR_DIFFUSE_VOLUME_H_PAYLOAD_CRC) !=
          vkr_crc32(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES,
                    size - VKR_DIFFUSE_VOLUME_HEADER_BYTES)) {
    return false_v;
  }
  volume.probe_count = volume.brick_count * VKR_DIFFUSE_VOLUME_BRICK_PROBES;

  const Scratch scope = scratch_create(arena);
  VkrLightLayer *layers = (VkrLightLayer *)arena_alloc(
      arena, (uint64_t)volume.layer_count * sizeof(*layers),
      ARENA_MEMORY_TAG_ARRAY);
  VkrDiffuseVolumeBrick *bricks = (VkrDiffuseVolumeBrick *)arena_alloc(
      arena, (uint64_t)volume.brick_count * sizeof(*bricks),
      ARENA_MEMORY_TAG_ARRAY);
  if (!layers || !bricks) {
    scratch_destroy(scope, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }
  for (uint32_t layer = 0u; layer < volume.layer_count; ++layer) {
    if (!vkr_light_layer_read(bytes + VKR_DIFFUSE_VOLUME_HEADER_BYTES +
                                  (uint64_t)layer *
                                      VKR_LIGHT_LAYER_RECORD_BYTES,
                              &layers[layer])) {
      scratch_destroy(scope, ARENA_MEMORY_TAG_ARRAY);
      return false_v;
    }
  }
  for (uint32_t b = 0u; b < volume.brick_count; ++b) {
    const uint8_t *record = bytes + sections.brick_offset +
                            (uint64_t)b * VKR_DIFFUSE_VOLUME_BRICK_RECORD_BYTES;
    for (uint32_t axis = 0u; axis < 3u; ++axis) {
      bricks[b].entry[axis] = vkr_load_le_u32(record + axis * sizeof(uint32_t));
    }
    bricks[b].level = vkr_load_le_u32(record + 12u);
  }
  volume.layers = layers;
  volume.bricks = bricks;
  volume.entries = bytes + sections.entry_offset;
  volume.probe_aux = bytes + sections.aux_offset;
  volume.moments = bytes + sections.moment_offset;
  volume.layer_sh = bytes + sections.sh_offset;
  if (!vkr_diffuse_volume_valid(&volume)) {
    scratch_destroy(scope, ARENA_MEMORY_TAG_ARRAY);
    return false_v;
  }
  *out_volume = volume;
  return true_v;
}
