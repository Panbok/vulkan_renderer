#include "renderer/resources/loaders/vkr_font_cooked.h"

#include "containers/bitset.h"
#include "core/vkr_atomic.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(CHAR_BIT == 8, "VKFA requires 8-bit bytes");
_Static_assert(sizeof(float32_t) == 4u, "VKFA requires 32-bit float32_t");
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "VKFA requires binary32 arithmetic");

static VkrAtomicUint64 s_vkr_font_cooked_temporary_counter = 0u;

enum {
  VKR_FONT_COOKED_H_MAGIC = 0u,
  VKR_FONT_COOKED_H_VERSION = 4u,
  VKR_FONT_COOKED_H_ENDIAN = 8u,
  VKR_FONT_COOKED_H_SIZE = 12u,
  VKR_FONT_COOKED_H_FLAGS = 16u,
  VKR_FONT_COOKED_H_FIELD = 20u,
  VKR_FONT_COOKED_H_FALLBACK = 24u,
  VKR_FONT_COOKED_H_COOKER = 28u,
  VKR_FONT_COOKED_H_GLYPHS = 32u,
  VKR_FONT_COOKED_H_CODEPOINTS = 36u,
  VKR_FONT_COOKED_H_KERNINGS = 40u,
  VKR_FONT_COOKED_H_PAGES = 44u,
  VKR_FONT_COOKED_H_FILE_SIZE = 48u,
  VKR_FONT_COOKED_H_FACE_SIZE = 56u,
  VKR_FONT_COOKED_H_IDENTITY = 64u,
  VKR_FONT_COOKED_H_METRICS = 96u,
};

typedef struct VkrFontCookedSectionView {
  uint32_t kind;
  uint64_t offset;
  uint64_t size;
  uint32_t crc;
} VkrFontCookedSectionView;

typedef struct VkrFontCookedLayout {
  uint32_t flags;
  uint32_t field_kind;
  uint32_t fallback_glyph_id;
  uint32_t cooker_version;
  uint32_t glyph_count;
  uint32_t codepoint_count;
  uint32_t kerning_count;
  uint32_t page_count;
  uint64_t file_size;
  uint32_t face_size;
  uint8_t identity[32];
  VkrFontCookedMetrics metrics;
  VkrFontCookedSectionView sections[VKR_FONT_COOKED_SECTION_COUNT];
} VkrFontCookedLayout;

typedef struct VkrFontCookedWriter {
  uint8_t *data;
  uint64_t size;
  uint64_t offset;
} VkrFontCookedWriter;

static bool8_t vkr_font_cooked_add(uint64_t a, uint64_t b, uint64_t *out) {
  if (a > UINT64_MAX - b) {
    return false_v;
  }
  *out = a + b;
  return true_v;
}

static bool8_t vkr_font_cooked_mul(uint64_t a, uint64_t b, uint64_t *out) {
  if (a != 0u && b > UINT64_MAX / a) {
    return false_v;
  }
  *out = a * b;
  return true_v;
}

static uint64_t vkr_font_cooked_align(uint64_t value) {
  const uint64_t mask = VKR_FONT_COOKED_ALIGNMENT - 1u;
  return (value + mask) & ~mask;
}

/**
 * VKFA freezes IEEE-754 binary32 bit patterns, not merely C's numeric range.
 * The FLT_* constraints above do not prove the object representation on every
 * conforming C11 implementation, so reject unsupported targets at this cold
 * serialization boundary before copying any float bits.
 */
static bool8_t vkr_font_cooked_float_representation_supported(void) {
  const float32_t one = 1.0f;
  const float32_t negative_half = -0.5f;
  uint32_t one_bits = 0u;
  uint32_t negative_half_bits = 0u;
  MemCopy(&one_bits, &one, sizeof(one_bits));
  MemCopy(&negative_half_bits, &negative_half, sizeof(negative_half_bits));
  return one_bits == 0x3f800000u && negative_half_bits == 0xbf000000u;
}

static uint32_t vkr_font_cooked_crc32(const uint8_t *data, uint64_t size) {
  uint32_t crc = 0xffffffffu;
  for (uint64_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint32_t bit = 0; bit < 8u; ++bit) {
      crc = (crc >> 1u) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  return ~crc;
}

static void vkr_font_cooked_write_u32(uint8_t *dst, uint32_t value) {
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8u);
  dst[2] = (uint8_t)(value >> 16u);
  dst[3] = (uint8_t)(value >> 24u);
}

static void vkr_font_cooked_write_u64(uint8_t *dst, uint64_t value) {
  vkr_font_cooked_write_u32(dst, (uint32_t)value);
  vkr_font_cooked_write_u32(dst + 4u, (uint32_t)(value >> 32u));
}

static uint32_t vkr_font_cooked_read_u32(const uint8_t *src) {
  return (uint32_t)src[0] | ((uint32_t)src[1] << 8u) |
         ((uint32_t)src[2] << 16u) | ((uint32_t)src[3] << 24u);
}

static uint64_t vkr_font_cooked_read_u64(const uint8_t *src) {
  return (uint64_t)vkr_font_cooked_read_u32(src) |
         ((uint64_t)vkr_font_cooked_read_u32(src + 4u) << 32u);
}

static void vkr_font_cooked_write_f32(uint8_t *dst, float32_t value) {
  uint32_t bits = 0;
  MemCopy(&bits, &value, sizeof(bits));
  vkr_font_cooked_write_u32(dst, bits);
}

static float32_t vkr_font_cooked_read_f32(const uint8_t *src) {
  uint32_t bits = vkr_font_cooked_read_u32(src);
  float32_t value = 0.0f;
  MemCopy(&value, &bits, sizeof(value));
  return value;
}

static bool8_t vkr_font_cooked_writer_bytes(VkrFontCookedWriter *writer,
                                            const void *data, uint64_t size) {
  if (writer->offset > writer->size || size > writer->size - writer->offset) {
    return false_v;
  }
  if (size != 0u) {
    MemCopy(writer->data + writer->offset, data, size);
  }
  writer->offset += size;
  return true_v;
}

static bool8_t vkr_font_cooked_writer_u32(VkrFontCookedWriter *writer,
                                          uint32_t value) {
  uint8_t bytes[4];
  vkr_font_cooked_write_u32(bytes, value);
  return vkr_font_cooked_writer_bytes(writer, bytes, sizeof(bytes));
}

static bool8_t vkr_font_cooked_writer_u64(VkrFontCookedWriter *writer,
                                          uint64_t value) {
  uint8_t bytes[8];
  vkr_font_cooked_write_u64(bytes, value);
  return vkr_font_cooked_writer_bytes(writer, bytes, sizeof(bytes));
}

static bool8_t vkr_font_cooked_writer_f32(VkrFontCookedWriter *writer,
                                          float32_t value) {
  uint8_t bytes[4];
  vkr_font_cooked_write_f32(bytes, value);
  return vkr_font_cooked_writer_bytes(writer, bytes, sizeof(bytes));
}

static bool8_t vkr_font_cooked_finite(float32_t value) {
  return isfinite(value) ? true_v : false_v;
}

static bool8_t
vkr_font_cooked_info_find_glyph(const VkrFontCookedEncodeInfo *info,
                                uint32_t glyph_id) {
  uint32_t first = 0u;
  uint32_t count = info->glyph_count;
  while (count != 0u) {
    const uint32_t step = count / 2u;
    const uint32_t index = first + step;
    const uint32_t candidate = info->glyphs[index].glyph_id;
    if (candidate < glyph_id) {
      first = index + 1u;
      count -= step + 1u;
    } else if (candidate > glyph_id) {
      count = step;
    } else {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t
vkr_font_cooked_validate_info(const VkrFontCookedEncodeInfo *info) {
  if (!vkr_font_cooked_float_representation_supported() || !info ||
      !info->face.str || info->face.length == 0u ||
      info->face.length > VKR_FONT_COOKED_MAX_FACE_BYTES ||
      info->cooker_version == 0u ||
      info->field_kind != VKR_FONT_COOKED_FIELD_MTSDF ||
      info->glyph_count == 0u ||
      info->glyph_count > VKR_FONT_COOKED_MAX_GLYPHS ||
      info->codepoint_count == 0u ||
      info->codepoint_count > VKR_FONT_COOKED_MAX_CODEPOINTS ||
      info->kerning_count > VKR_FONT_COOKED_MAX_KERNINGS ||
      info->page_count != 1u || info->page_count > VKR_FONT_COOKED_MAX_PAGES ||
      !info->glyphs || !info->pages ||
      (!info->codepoints && info->codepoint_count != 0u) ||
      (!info->kernings && info->kerning_count != 0u)) {
    return false_v;
  }
  const VkrFontCookedMetrics *m = &info->metrics;
  if (!vkr_font_cooked_finite(m->line_height) ||
      !vkr_font_cooked_finite(m->ascender) ||
      !vkr_font_cooked_finite(m->descender) ||
      !vkr_font_cooked_finite(m->underline_y) ||
      !vkr_font_cooked_finite(m->underline_thickness) ||
      !vkr_font_cooked_finite(m->distance_range) ||
      !vkr_font_cooked_finite(m->atlas_px_per_em) || m->line_height <= 0.0f ||
      m->underline_thickness <= 0.0f || m->distance_range <= 0.0f ||
      m->atlas_px_per_em <= 0.0f || m->units_per_em == 0u) {
    return false_v;
  }
  if (info->pages[0].width == 0u || info->pages[0].height == 0u ||
      info->pages[0].pixel_format != VKR_FONT_COOKED_PIXEL_RGBA8_UNORM ||
      info->pages[0].row_stride != (uint64_t)info->pages[0].width * 4u ||
      info->pages[0].pixel_size !=
          (uint64_t)info->pages[0].row_stride * info->pages[0].height ||
      !info->pages[0].pixels || info->pages[0].pixel_size == 0u) {
    return false_v;
  }
  uint32_t previous = 0u;
  for (uint32_t i = 0; i < info->glyph_count; ++i) {
    const VkrFontCookedGlyph *g = &info->glyphs[i];
    if ((i != 0u && g->glyph_id <= previous) || g->page_index != 0u ||
        (g->flags & ~VKR_FONT_COOKED_GLYPH_HAS_GEOMETRY) != 0u ||
        !vkr_font_cooked_finite(g->advance) ||
        !vkr_font_cooked_finite(g->plane_left) ||
        !vkr_font_cooked_finite(g->plane_bottom) ||
        !vkr_font_cooked_finite(g->plane_right) ||
        !vkr_font_cooked_finite(g->plane_top) ||
        !vkr_font_cooked_finite(g->uv_left) ||
        !vkr_font_cooked_finite(g->uv_bottom) ||
        !vkr_font_cooked_finite(g->uv_right) ||
        !vkr_font_cooked_finite(g->uv_top) || g->plane_left > g->plane_right ||
        g->plane_bottom > g->plane_top || g->uv_left < 0.0f ||
        g->uv_bottom < 0.0f || g->uv_right > 1.0f || g->uv_top > 1.0f ||
        g->uv_left > g->uv_right || g->uv_bottom > g->uv_top) {
      return false_v;
    }
    previous = g->glyph_id;
  }
  if (!vkr_font_cooked_info_find_glyph(info, info->fallback_glyph_id)) {
    return false_v;
  }
  previous = 0u;
  for (uint32_t i = 0; i < info->codepoint_count; ++i) {
    const VkrFontCookedCodepoint *cp = &info->codepoints[i];
    if ((i != 0u && cp->codepoint <= previous) || cp->codepoint > 0x10ffffu ||
        (cp->codepoint >= 0xd800u && cp->codepoint <= 0xdfffu)) {
      return false_v;
    }
    if (!vkr_font_cooked_info_find_glyph(info, cp->glyph_id)) {
      return false_v;
    }
    previous = cp->codepoint;
  }
  uint32_t previous_left = 0u, previous_right = 0u;
  for (uint32_t i = 0; i < info->kerning_count; ++i) {
    const VkrFontCookedKerning *k = &info->kernings[i];
    if ((i != 0u && (k->left_glyph_id < previous_left ||
                     (k->left_glyph_id == previous_left &&
                      k->right_glyph_id <= previous_right))) ||
        !vkr_font_cooked_finite(k->amount)) {
      return false_v;
    }
    if (!vkr_font_cooked_info_find_glyph(info, k->left_glyph_id) ||
        !vkr_font_cooked_info_find_glyph(info, k->right_glyph_id)) {
      return false_v;
    }
    previous_left = k->left_glyph_id;
    previous_right = k->right_glyph_id;
  }
  return true_v;
}

static bool8_t vkr_font_cooked_parse_layout(const uint8_t *data, uint64_t size,
                                            VkrFontCookedLayout *out) {
  if (!vkr_font_cooked_float_representation_supported() || !data || !out ||
      size < VKR_FONT_COOKED_DATA_OFFSET ||
      size > VKR_FONT_COOKED_MAX_FILE_SIZE) {
    return false_v;
  }
  if (vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_MAGIC) !=
          VKR_FONT_COOKED_MAGIC ||
      vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_VERSION) !=
          VKR_FONT_COOKED_VERSION ||
      vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_ENDIAN) !=
          VKR_FONT_COOKED_ENDIAN_TAG ||
      vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_SIZE) !=
          VKR_FONT_COOKED_HEADER_SIZE) {
    return false_v;
  }
  uint8_t header[VKR_FONT_COOKED_HEADER_SIZE];
  MemCopy(header, data, sizeof(header));
  const uint32_t stored_header_crc =
      vkr_font_cooked_read_u32(header + VKR_FONT_COOKED_HEADER_CRC_OFFSET);
  MemZero(header + VKR_FONT_COOKED_HEADER_CRC_OFFSET, sizeof(uint32_t));
  if (stored_header_crc != vkr_font_cooked_crc32(header, sizeof(header))) {
    return false_v;
  }
  const uint64_t file_size =
      vkr_font_cooked_read_u64(data + VKR_FONT_COOKED_H_FILE_SIZE);
  if (file_size != size || file_size < VKR_FONT_COOKED_DATA_OFFSET) {
    return false_v;
  }
  if (vkr_font_cooked_read_u32(data + 60u) != 0u ||
      vkr_font_cooked_read_u32(data + 128u) != 0u) {
    return false_v;
  }
  for (uint32_t i = 144u; i < VKR_FONT_COOKED_HEADER_SIZE; ++i) {
    if (data[i] != 0u) {
      return false_v;
    }
  }
  MemZero(out, sizeof(*out));
  out->flags = vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_FLAGS);
  out->field_kind = vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_FIELD);
  out->fallback_glyph_id =
      vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_FALLBACK);
  out->cooker_version =
      vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_COOKER);
  out->glyph_count = vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_GLYPHS);
  out->codepoint_count =
      vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_CODEPOINTS);
  out->kerning_count =
      vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_KERNINGS);
  out->page_count = vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_PAGES);
  out->file_size = file_size;
  out->face_size = vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_H_FACE_SIZE);
  MemCopy(out->identity, data + VKR_FONT_COOKED_H_IDENTITY, 32u);
  uint8_t *metrics = (uint8_t *)data + VKR_FONT_COOKED_H_METRICS;
  out->metrics.line_height = vkr_font_cooked_read_f32(metrics + 0u);
  out->metrics.ascender = vkr_font_cooked_read_f32(metrics + 4u);
  out->metrics.descender = vkr_font_cooked_read_f32(metrics + 8u);
  out->metrics.underline_y = vkr_font_cooked_read_f32(metrics + 12u);
  out->metrics.underline_thickness = vkr_font_cooked_read_f32(metrics + 16u);
  out->metrics.distance_range = vkr_font_cooked_read_f32(metrics + 20u);
  out->metrics.atlas_px_per_em = vkr_font_cooked_read_f32(metrics + 24u);
  out->metrics.units_per_em = vkr_font_cooked_read_u32(metrics + 28u);
  const uint32_t stored_directory_crc =
      vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_DIRECTORY_CRC_OFFSET);
  if (stored_directory_crc !=
      vkr_font_cooked_crc32(data + VKR_FONT_COOKED_DIRECTORY_OFFSET,
                            VKR_FONT_COOKED_DIRECTORY_SIZE)) {
    return false_v;
  }
  const uint32_t stored_payload_crc =
      vkr_font_cooked_read_u32(data + VKR_FONT_COOKED_PAYLOAD_CRC_OFFSET);
  if (stored_payload_crc !=
      vkr_font_cooked_crc32(data + VKR_FONT_COOKED_DATA_OFFSET,
                            size - VKR_FONT_COOKED_DATA_OFFSET)) {
    return false_v;
  }
  for (uint32_t i = 0; i < VKR_FONT_COOKED_SECTION_COUNT; ++i) {
    const uint8_t *entry = data + VKR_FONT_COOKED_DIRECTORY_OFFSET +
                           i * VKR_FONT_COOKED_SECTION_SIZE;
    VkrFontCookedSectionView *section = &out->sections[i];
    section->kind = vkr_font_cooked_read_u32(entry);
    section->offset = vkr_font_cooked_read_u64(entry + 8u);
    section->size = vkr_font_cooked_read_u64(entry + 16u);
    section->crc = vkr_font_cooked_read_u32(entry + 24u);
    if (section->kind != i + 1u || vkr_font_cooked_read_u32(entry + 4u) != 0u ||
        vkr_font_cooked_read_u32(entry + 28u) != 0u ||
        (section->offset & (VKR_FONT_COOKED_ALIGNMENT - 1u)) != 0u ||
        section->offset < VKR_FONT_COOKED_DATA_OFFSET ||
        section->offset > size || section->size > size - section->offset ||
        section->crc !=
            vkr_font_cooked_crc32(data + section->offset, section->size)) {
      return false_v;
    }
  }
  uint64_t payload_end = VKR_FONT_COOKED_DATA_OFFSET;
  for (uint32_t i = 0; i < VKR_FONT_COOKED_SECTION_COUNT; ++i) {
    const uint64_t expected_offset = vkr_font_cooked_align(payload_end);
    if (out->sections[i].offset != expected_offset) {
      return false_v;
    }
    for (uint64_t padding = payload_end; padding < expected_offset; ++padding) {
      if (data[padding] != 0u) {
        return false_v;
      }
    }
    uint64_t section_end = 0u;
    if (!vkr_font_cooked_add(out->sections[i].offset, out->sections[i].size,
                             &section_end)) {
      return false_v;
    }
    payload_end = section_end;
  }
  const uint64_t aligned_payload_end = vkr_font_cooked_align(payload_end);
  if (file_size != aligned_payload_end) {
    return false_v;
  }
  for (uint64_t padding = payload_end; padding < aligned_payload_end;
       ++padding) {
    if (data[padding] != 0u) {
      return false_v;
    }
  }
  uint64_t expected = 0;
  if (!vkr_font_cooked_mul(out->glyph_count, VKR_FONT_COOKED_GLYPH_SIZE,
                           &expected) ||
      out->sections[1].size != expected ||
      !vkr_font_cooked_mul(out->codepoint_count, VKR_FONT_COOKED_CODEPOINT_SIZE,
                           &expected) ||
      out->sections[2].size != expected ||
      !vkr_font_cooked_mul(out->kerning_count, VKR_FONT_COOKED_KERNING_SIZE,
                           &expected) ||
      out->sections[3].size != expected ||
      !vkr_font_cooked_mul(out->page_count, VKR_FONT_COOKED_PAGE_SIZE,
                           &expected) ||
      out->sections[4].size != expected ||
      out->sections[0].size != out->face_size || out->face_size == 0u ||
      out->face_size > VKR_FONT_COOKED_MAX_FACE_BYTES ||
      out->cooker_version == 0u || out->glyph_count == 0u ||
      out->glyph_count > VKR_FONT_COOKED_MAX_GLYPHS ||
      out->codepoint_count == 0u ||
      out->codepoint_count > VKR_FONT_COOKED_MAX_CODEPOINTS ||
      out->kerning_count > VKR_FONT_COOKED_MAX_KERNINGS ||
      out->page_count > VKR_FONT_COOKED_MAX_PAGES) {
    return false_v;
  }
  return true_v;
}

static bool8_t vkr_font_cooked_find_glyph(const uint8_t *data,
                                          const VkrFontCookedLayout *layout,
                                          uint32_t glyph_id) {
  const uint8_t *records = data + layout->sections[1].offset;
  uint32_t first = 0u;
  uint32_t count = layout->glyph_count;
  while (count != 0u) {
    const uint32_t step = count / 2u;
    const uint32_t index = first + step;
    const uint32_t candidate = vkr_font_cooked_read_u32(
        records + (uint64_t)index * VKR_FONT_COOKED_GLYPH_SIZE);
    if (candidate < glyph_id) {
      first = index + 1u;
      count -= step + 1u;
    } else if (candidate > glyph_id) {
      count = step;
    } else {
      return true_v;
    }
  }
  return false_v;
}

static bool8_t
vkr_font_cooked_validate_semantics(const uint8_t *data,
                                   const VkrFontCookedLayout *layout) {
  if (layout->flags != 0u ||
      layout->field_kind != VKR_FONT_COOKED_FIELD_MTSDF ||
      layout->page_count != 1u ||
      !vkr_font_cooked_finite(layout->metrics.line_height) ||
      !vkr_font_cooked_finite(layout->metrics.ascender) ||
      !vkr_font_cooked_finite(layout->metrics.descender) ||
      !vkr_font_cooked_finite(layout->metrics.underline_y) ||
      !vkr_font_cooked_finite(layout->metrics.underline_thickness) ||
      !vkr_font_cooked_finite(layout->metrics.distance_range) ||
      !vkr_font_cooked_finite(layout->metrics.atlas_px_per_em) ||
      layout->metrics.line_height <= 0.0f ||
      layout->metrics.underline_thickness <= 0.0f ||
      layout->metrics.distance_range <= 0.0f ||
      layout->metrics.atlas_px_per_em <= 0.0f ||
      layout->metrics.units_per_em == 0u ||
      !vkr_font_cooked_find_glyph(data, layout, layout->fallback_glyph_id)) {
    return false_v;
  }
  const uint8_t *glyphs = data + layout->sections[1].offset;
  uint32_t previous = 0u;
  for (uint32_t i = 0; i < layout->glyph_count; ++i) {
    const uint8_t *g = glyphs + i * VKR_FONT_COOKED_GLYPH_SIZE;
    const uint32_t id = vkr_font_cooked_read_u32(g);
    const uint32_t page = vkr_font_cooked_read_u32(g + 4u);
    const uint32_t flags = vkr_font_cooked_read_u32(g + 8u);
    float32_t values[9];
    for (uint32_t j = 0; j < 9u; ++j) {
      values[j] = vkr_font_cooked_read_f32(g + 12u + j * 4u);
      if (!vkr_font_cooked_finite(values[j])) {
        return false_v;
      }
    }
    if ((i != 0u && id <= previous) || page >= layout->page_count ||
        (flags & ~VKR_FONT_COOKED_GLYPH_HAS_GEOMETRY) != 0u ||
        values[1] > values[3] || values[2] > values[4] || values[5] < 0.0f ||
        values[6] < 0.0f || values[7] > 1.0f || values[8] > 1.0f ||
        values[5] > values[7] || values[6] > values[8]) {
      return false_v;
    }
    previous = id;
  }
  const uint8_t *codepoints = data + layout->sections[2].offset;
  previous = 0u;
  for (uint32_t i = 0; i < layout->codepoint_count; ++i) {
    const uint8_t *cp = codepoints + i * VKR_FONT_COOKED_CODEPOINT_SIZE;
    const uint32_t codepoint = vkr_font_cooked_read_u32(cp);
    if ((i != 0u && codepoint <= previous) || codepoint > 0x10ffffu ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
        !vkr_font_cooked_find_glyph(data, layout,
                                    vkr_font_cooked_read_u32(cp + 4u))) {
      return false_v;
    }
    previous = codepoint;
  }
  const uint8_t *kernings = data + layout->sections[3].offset;
  uint32_t prior_left = 0u, prior_right = 0u;
  for (uint32_t i = 0; i < layout->kerning_count; ++i) {
    const uint8_t *k = kernings + i * VKR_FONT_COOKED_KERNING_SIZE;
    const uint32_t left = vkr_font_cooked_read_u32(k);
    const uint32_t right = vkr_font_cooked_read_u32(k + 4u);
    if ((i != 0u &&
         (left < prior_left || (left == prior_left && right <= prior_right))) ||
        !vkr_font_cooked_find_glyph(data, layout, left) ||
        !vkr_font_cooked_find_glyph(data, layout, right) ||
        !vkr_font_cooked_finite(vkr_font_cooked_read_f32(k + 8u)) ||
        vkr_font_cooked_read_u32(k + 12u) != 0u) {
      return false_v;
    }
    prior_left = left;
    prior_right = right;
  }
  const uint8_t *pages = data + layout->sections[4].offset;
  uint64_t covered_pixels = 0u;
  for (uint32_t i = 0; i < layout->page_count; ++i) {
    const uint8_t *p = pages + i * VKR_FONT_COOKED_PAGE_SIZE;
    const uint32_t width = vkr_font_cooked_read_u32(p);
    const uint32_t height = vkr_font_cooked_read_u32(p + 4u);
    const uint32_t stride = vkr_font_cooked_read_u32(p + 8u);
    const uint32_t format = vkr_font_cooked_read_u32(p + 12u);
    const uint64_t offset = vkr_font_cooked_read_u64(p + 16u);
    const uint64_t pixel_size = vkr_font_cooked_read_u64(p + 24u);
    if (width == 0u || height == 0u ||
        format != VKR_FONT_COOKED_PIXEL_RGBA8_UNORM ||
        stride != (uint64_t)width * 4u ||
        pixel_size != (uint64_t)stride * height || pixel_size == 0u ||
        offset != layout->sections[5].offset + covered_pixels ||
        offset > layout->file_size || pixel_size > layout->file_size - offset ||
        vkr_font_cooked_read_u32(p + 36u) != 0u ||
        vkr_font_cooked_read_u32(p + 40u) != 0u ||
        vkr_font_cooked_read_u32(p + 44u) != 0u ||
        vkr_font_cooked_read_u32(p + 32u) !=
            vkr_font_cooked_crc32(data + offset, pixel_size)) {
      return false_v;
    }
    covered_pixels += pixel_size;
  }
  return covered_pixels == layout->sections[5].size;
}

bool8_t vkr_font_cooked_encode(VkrAllocator *scratch_allocator,
                               const VkrFontCookedEncodeInfo *info,
                               uint8_t **out_data, uint64_t *out_size) {
  if (!out_data || !out_size) {
    return false_v;
  }
  *out_data = NULL;
  *out_size = 0u;
  if (!scratch_allocator || !vkr_font_cooked_validate_info(info)) {
    return false_v;
  }
  uint64_t section_sizes[VKR_FONT_COOKED_SECTION_COUNT] = {
      info->face.length,
      (uint64_t)info->glyph_count * VKR_FONT_COOKED_GLYPH_SIZE,
      (uint64_t)info->codepoint_count * VKR_FONT_COOKED_CODEPOINT_SIZE,
      (uint64_t)info->kerning_count * VKR_FONT_COOKED_KERNING_SIZE,
      (uint64_t)info->page_count * VKR_FONT_COOKED_PAGE_SIZE,
      info->pages[0].pixel_size,
  };
  uint64_t offsets[VKR_FONT_COOKED_SECTION_COUNT];
  uint64_t cursor = VKR_FONT_COOKED_DATA_OFFSET;
  for (uint32_t i = 0; i < VKR_FONT_COOKED_SECTION_COUNT; ++i) {
    cursor = vkr_font_cooked_align(cursor);
    offsets[i] = cursor;
    if (!vkr_font_cooked_add(cursor, section_sizes[i], &cursor)) {
      return false_v;
    }
  }
  const uint64_t file_size = vkr_font_cooked_align(cursor);
  if (file_size > VKR_FONT_COOKED_MAX_FILE_SIZE || file_size > SIZE_MAX) {
    return false_v;
  }
  uint8_t *artifact = vkr_allocator_alloc(scratch_allocator, file_size,
                                          VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!artifact) {
    return false_v;
  }
  MemZero(artifact, file_size);
  vkr_font_cooked_write_u32(artifact + 0u, VKR_FONT_COOKED_MAGIC);
  vkr_font_cooked_write_u32(artifact + 4u, VKR_FONT_COOKED_VERSION);
  vkr_font_cooked_write_u32(artifact + 8u, VKR_FONT_COOKED_ENDIAN_TAG);
  vkr_font_cooked_write_u32(artifact + 12u, VKR_FONT_COOKED_HEADER_SIZE);
  vkr_font_cooked_write_u32(artifact + 16u, 0u);
  vkr_font_cooked_write_u32(artifact + 20u, info->field_kind);
  vkr_font_cooked_write_u32(artifact + 24u, info->fallback_glyph_id);
  vkr_font_cooked_write_u32(artifact + 28u, info->cooker_version);
  vkr_font_cooked_write_u32(artifact + 32u, info->glyph_count);
  vkr_font_cooked_write_u32(artifact + 36u, info->codepoint_count);
  vkr_font_cooked_write_u32(artifact + 40u, info->kerning_count);
  vkr_font_cooked_write_u32(artifact + 44u, info->page_count);
  vkr_font_cooked_write_u64(artifact + 48u, file_size);
  vkr_font_cooked_write_u32(artifact + 56u, (uint32_t)info->face.length);
  MemCopy(artifact + 64u, info->identity, 32u);
  vkr_font_cooked_write_f32(artifact + 96u, info->metrics.line_height);
  vkr_font_cooked_write_f32(artifact + 100u, info->metrics.ascender);
  vkr_font_cooked_write_f32(artifact + 104u, info->metrics.descender);
  vkr_font_cooked_write_f32(artifact + 108u, info->metrics.underline_y);
  vkr_font_cooked_write_f32(artifact + 112u, info->metrics.underline_thickness);
  vkr_font_cooked_write_f32(artifact + 116u, info->metrics.distance_range);
  vkr_font_cooked_write_f32(artifact + 120u, info->metrics.atlas_px_per_em);
  vkr_font_cooked_write_u32(artifact + 124u, info->metrics.units_per_em);
  VkrFontCookedWriter writer = {
      .data = artifact, .size = file_size, .offset = 0};
  for (uint32_t i = 0; i < VKR_FONT_COOKED_SECTION_COUNT; ++i) {
    uint8_t *entry = artifact + VKR_FONT_COOKED_DIRECTORY_OFFSET +
                     i * VKR_FONT_COOKED_SECTION_SIZE;
    vkr_font_cooked_write_u32(entry, i + 1u);
    vkr_font_cooked_write_u64(entry + 8u, offsets[i]);
    vkr_font_cooked_write_u64(entry + 16u, section_sizes[i]);
  }
  writer.offset = offsets[0];
  if (!vkr_font_cooked_writer_bytes(&writer, info->face.str, info->face.length))
    return false_v;
  writer.offset = offsets[1];
  for (uint32_t i = 0; i < info->glyph_count; ++i) {
    const VkrFontCookedGlyph *g = &info->glyphs[i];
    if (!vkr_font_cooked_writer_u32(&writer, g->glyph_id) ||
        !vkr_font_cooked_writer_u32(&writer, g->page_index) ||
        !vkr_font_cooked_writer_u32(&writer, g->flags) ||
        !vkr_font_cooked_writer_f32(&writer, g->advance) ||
        !vkr_font_cooked_writer_f32(&writer, g->plane_left) ||
        !vkr_font_cooked_writer_f32(&writer, g->plane_bottom) ||
        !vkr_font_cooked_writer_f32(&writer, g->plane_right) ||
        !vkr_font_cooked_writer_f32(&writer, g->plane_top) ||
        !vkr_font_cooked_writer_f32(&writer, g->uv_left) ||
        !vkr_font_cooked_writer_f32(&writer, g->uv_bottom) ||
        !vkr_font_cooked_writer_f32(&writer, g->uv_right) ||
        !vkr_font_cooked_writer_f32(&writer, g->uv_top))
      return false_v;
  }
  writer.offset = offsets[2];
  for (uint32_t i = 0; i < info->codepoint_count; ++i) {
    if (!vkr_font_cooked_writer_u32(&writer, info->codepoints[i].codepoint) ||
        !vkr_font_cooked_writer_u32(&writer, info->codepoints[i].glyph_id))
      return false_v;
  }
  writer.offset = offsets[3];
  for (uint32_t i = 0; i < info->kerning_count; ++i) {
    const VkrFontCookedKerning *k = &info->kernings[i];
    if (!vkr_font_cooked_writer_u32(&writer, k->left_glyph_id) ||
        !vkr_font_cooked_writer_u32(&writer, k->right_glyph_id) ||
        !vkr_font_cooked_writer_f32(&writer, k->amount) ||
        !vkr_font_cooked_writer_u32(&writer, 0u))
      return false_v;
  }
  writer.offset = offsets[4];
  const VkrFontCookedPage *page = &info->pages[0];
  if (!vkr_font_cooked_writer_u32(&writer, page->width) ||
      !vkr_font_cooked_writer_u32(&writer, page->height) ||
      !vkr_font_cooked_writer_u32(&writer, page->row_stride) ||
      !vkr_font_cooked_writer_u32(&writer, page->pixel_format) ||
      !vkr_font_cooked_writer_u64(&writer, offsets[5]) ||
      !vkr_font_cooked_writer_u64(&writer, page->pixel_size) ||
      !vkr_font_cooked_writer_u32(
          &writer, vkr_font_cooked_crc32(page->pixels, page->pixel_size)) ||
      !vkr_font_cooked_writer_u32(&writer, 0u) ||
      !vkr_font_cooked_writer_u32(&writer, 0u) ||
      !vkr_font_cooked_writer_u32(&writer, 0u))
    return false_v;
  writer.offset = offsets[5];
  if (!vkr_font_cooked_writer_bytes(&writer, page->pixels, page->pixel_size))
    return false_v;
  for (uint32_t i = 0; i < VKR_FONT_COOKED_SECTION_COUNT; ++i) {
    uint8_t *entry = artifact + VKR_FONT_COOKED_DIRECTORY_OFFSET +
                     i * VKR_FONT_COOKED_SECTION_SIZE;
    vkr_font_cooked_write_u32(
        entry + 24u,
        vkr_font_cooked_crc32(artifact + offsets[i], section_sizes[i]));
  }
  const uint32_t directory_crc =
      vkr_font_cooked_crc32(artifact + VKR_FONT_COOKED_DIRECTORY_OFFSET,
                            VKR_FONT_COOKED_DIRECTORY_SIZE);
  vkr_font_cooked_write_u32(artifact + VKR_FONT_COOKED_DIRECTORY_CRC_OFFSET,
                            directory_crc);
  const uint32_t payload_crc =
      vkr_font_cooked_crc32(artifact + VKR_FONT_COOKED_DATA_OFFSET,
                            file_size - VKR_FONT_COOKED_DATA_OFFSET);
  vkr_font_cooked_write_u32(artifact + VKR_FONT_COOKED_PAYLOAD_CRC_OFFSET,
                            payload_crc);
  vkr_font_cooked_write_u32(artifact + VKR_FONT_COOKED_HEADER_CRC_OFFSET, 0u);
  vkr_font_cooked_write_u32(
      artifact + VKR_FONT_COOKED_HEADER_CRC_OFFSET,
      vkr_font_cooked_crc32(artifact, VKR_FONT_COOKED_HEADER_SIZE));
  *out_data = artifact;
  *out_size = file_size;
  return true_v;
}

bool8_t vkr_font_cooked_inspect(const uint8_t *data, uint64_t size,
                                VkrFontCookedInspection *out_inspection) {
  if (out_inspection)
    *out_inspection = (VkrFontCookedInspection){0};
  VkrFontCookedLayout layout;
  if (!out_inspection || !vkr_font_cooked_parse_layout(data, size, &layout) ||
      !vkr_font_cooked_validate_semantics(data, &layout))
    return false_v;
  MemCopy(out_inspection->identity, layout.identity, 32u);
  out_inspection->file_size = layout.file_size;
  out_inspection->cooker_version = layout.cooker_version;
  out_inspection->glyph_count = layout.glyph_count;
  out_inspection->codepoint_count = layout.codepoint_count;
  out_inspection->kerning_count = layout.kerning_count;
  out_inspection->page_count = layout.page_count;
  return true_v;
}

bool8_t vkr_font_cooked_decode(VkrAllocator *result_allocator,
                               const uint8_t *data, uint64_t size,
                               VkrFontCookedDecoded *out_decoded) {
  if (out_decoded)
    *out_decoded = (VkrFontCookedDecoded){0};
  VkrFontCookedLayout layout;
  if (!result_allocator || !out_decoded ||
      !vkr_font_cooked_parse_layout(data, size, &layout) ||
      !vkr_font_cooked_validate_semantics(data, &layout))
    return false_v;
  bool8_t ok = false_v;
  VkrFontCookedDecoded decoded = {0};
  decoded.face.length = layout.face_size;
  decoded.face.str = vkr_allocator_alloc(
      result_allocator, layout.face_size + 1u, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!decoded.face.str)
    goto cleanup;
  MemCopy(decoded.face.str, data + layout.sections[0].offset, layout.face_size);
  decoded.face.str[layout.face_size] = '\0';
  MemCopy(decoded.identity, layout.identity, 32u);
  decoded.cooker_version = layout.cooker_version;
  decoded.field_kind = (VkrFontCookedFieldKind)layout.field_kind;
  decoded.fallback_glyph_id = layout.fallback_glyph_id;
  decoded.metrics = layout.metrics;
  decoded.glyph_count = layout.glyph_count;
  decoded.codepoint_count = layout.codepoint_count;
  decoded.kerning_count = layout.kerning_count;
  decoded.page_count = layout.page_count;
  if (decoded.glyph_count != 0u) {
    decoded.glyphs = vkr_allocator_alloc(result_allocator,
                                         (uint64_t)decoded.glyph_count *
                                             sizeof(*decoded.glyphs),
                                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!decoded.glyphs)
      goto cleanup;
  }
  if (decoded.codepoint_count != 0u) {
    decoded.codepoints = vkr_allocator_alloc(result_allocator,
                                             (uint64_t)decoded.codepoint_count *
                                                 sizeof(*decoded.codepoints),
                                             VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!decoded.codepoints)
      goto cleanup;
  }
  if (decoded.kerning_count != 0u) {
    decoded.kernings = vkr_allocator_alloc(result_allocator,
                                           (uint64_t)decoded.kerning_count *
                                               sizeof(*decoded.kernings),
                                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!decoded.kernings)
      goto cleanup;
  }
  decoded.pages = vkr_allocator_alloc(
      result_allocator, (uint64_t)decoded.page_count * sizeof(*decoded.pages),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!decoded.pages)
    goto cleanup;
  const uint8_t *records = data + layout.sections[1].offset;
  for (uint32_t i = 0; i < decoded.glyph_count; ++i) {
    VkrFontCookedGlyph *g = &decoded.glyphs[i];
    const uint8_t *src = records + i * VKR_FONT_COOKED_GLYPH_SIZE;
    g->glyph_id = vkr_font_cooked_read_u32(src);
    g->page_index = vkr_font_cooked_read_u32(src + 4u);
    g->flags = vkr_font_cooked_read_u32(src + 8u);
    g->advance = vkr_font_cooked_read_f32(src + 12u);
    g->plane_left = vkr_font_cooked_read_f32(src + 16u);
    g->plane_bottom = vkr_font_cooked_read_f32(src + 20u);
    g->plane_right = vkr_font_cooked_read_f32(src + 24u);
    g->plane_top = vkr_font_cooked_read_f32(src + 28u);
    g->uv_left = vkr_font_cooked_read_f32(src + 32u);
    g->uv_bottom = vkr_font_cooked_read_f32(src + 36u);
    g->uv_right = vkr_font_cooked_read_f32(src + 40u);
    g->uv_top = vkr_font_cooked_read_f32(src + 44u);
  }
  records = data + layout.sections[2].offset;
  for (uint32_t i = 0; i < decoded.codepoint_count; ++i) {
    decoded.codepoints[i].codepoint =
        vkr_font_cooked_read_u32(records + i * 8u);
    decoded.codepoints[i].glyph_id =
        vkr_font_cooked_read_u32(records + i * 8u + 4u);
  }
  records = data + layout.sections[3].offset;
  for (uint32_t i = 0; i < decoded.kerning_count; ++i) {
    decoded.kernings[i].left_glyph_id =
        vkr_font_cooked_read_u32(records + i * 16u);
    decoded.kernings[i].right_glyph_id =
        vkr_font_cooked_read_u32(records + i * 16u + 4u);
    decoded.kernings[i].amount =
        vkr_font_cooked_read_f32(records + i * 16u + 8u);
  }
  records = data + layout.sections[4].offset;
  for (uint32_t i = 0; i < decoded.page_count; ++i) {
    VkrFontCookedPage *p = &decoded.pages[i];
    p->width = vkr_font_cooked_read_u32(records);
    p->height = vkr_font_cooked_read_u32(records + 4u);
    p->row_stride = vkr_font_cooked_read_u32(records + 8u);
    p->pixel_format =
        (VkrFontCookedPixelFormat)vkr_font_cooked_read_u32(records + 12u);
    const uint64_t pixel_offset = vkr_font_cooked_read_u64(records + 16u);
    p->pixel_size = vkr_font_cooked_read_u64(records + 24u);
    p->pixels = data + pixel_offset;
    records += VKR_FONT_COOKED_PAGE_SIZE;
  }
  ok = true_v;
cleanup:
  if (!ok) {
    if (decoded.pages)
      vkr_allocator_free(result_allocator, decoded.pages,
                         (uint64_t)decoded.page_count * sizeof(*decoded.pages),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (decoded.kernings)
      vkr_allocator_free(result_allocator, decoded.kernings,
                         (uint64_t)decoded.kerning_count *
                             sizeof(*decoded.kernings),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (decoded.codepoints)
      vkr_allocator_free(result_allocator, decoded.codepoints,
                         (uint64_t)decoded.codepoint_count *
                             sizeof(*decoded.codepoints),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (decoded.glyphs)
      vkr_allocator_free(result_allocator, decoded.glyphs,
                         (uint64_t)decoded.glyph_count *
                             sizeof(*decoded.glyphs),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (decoded.face.str)
      vkr_allocator_free(result_allocator, decoded.face.str,
                         layout.face_size + 1u,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }
  *out_decoded = decoded;
  return true_v;
}

static bool8_t vkr_font_cooked_path_is_absolute(String8 path) {
  return (path.length > 0u && (path.str[0] == '/' || path.str[0] == '\\')) ||
         (path.length > 1u && path.str[1] == ':');
}

bool8_t vkr_font_cooked_write_atomic(VkrAllocator *scratch_allocator,
                                     String8 output_path, const uint8_t *data,
                                     uint64_t size) {
  bool8_t ok = false_v;
  bool8_t temporary_owned = false_v;
  FileHandle file = {0};
  FilePath output = {0}, temporary = {0};
  if (!scratch_allocator || !output_path.str || output_path.length == 0u ||
      !data || size == 0u)
    goto cleanup;
  String8 directory = file_path_get_directory(scratch_allocator, output_path);
  if (directory.length > 0u &&
      !file_ensure_directory(scratch_allocator, &directory))
    goto cleanup;
  String8 output_nt = string8_duplicate(scratch_allocator, &output_path);
  if (!output_nt.str)
    goto cleanup;
  const FilePathType type = vkr_font_cooked_path_is_absolute(output_path)
                                ? FILE_PATH_TYPE_ABSOLUTE
                                : FILE_PATH_TYPE_RELATIVE;
  output = file_path_create(string8_cstr(&output_nt), scratch_allocator, type);
  if (!output.path.str)
    goto cleanup;
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_CREATE);
  bitset8_set(&mode, FILE_MODE_EXCLUSIVE);
  bitset8_set(&mode, FILE_MODE_BINARY);
  for (uint32_t attempt = 0u; attempt < 64u; ++attempt) {
    const uint64_t sequence = vkr_atomic_uint64_fetch_add(
        &s_vkr_font_cooked_temporary_counter, 1u, VKR_MEMORY_ORDER_RELAXED);
    String8 temporary_path = string8_create_formatted(
        scratch_allocator, "%.*s.tmp.%u.%llu", (int32_t)output_path.length,
        output_path.str, vkr_platform_get_process_id(),
        (unsigned long long)sequence);
    if (!temporary_path.str)
      goto cleanup;
    temporary = file_path_create(string8_cstr(&temporary_path),
                                 scratch_allocator, type);
    if (!temporary.path.str)
      goto cleanup;
    const FileError open_error = file_open(&temporary, mode, &file);
    if (open_error == FILE_ERROR_NONE) {
      temporary_owned = true_v;
      break;
    }
    temporary = (FilePath){0};
    if (open_error != FILE_ERROR_ALREADY_EXISTS)
      goto cleanup;
  }
  if (!temporary_owned)
    goto cleanup;
  uint64_t written = 0u;
  if (file_write(&file, size, data, &written) != FILE_ERROR_NONE ||
      written != size || file_sync(&file) != FILE_ERROR_NONE)
    goto cleanup;
  file_close(&file);
  if (file_rename(&temporary, &output, true_v) != FILE_ERROR_NONE)
    goto cleanup;
  temporary_owned = false_v;
  ok = true_v;
cleanup:
  file_close(&file);
  if (temporary_owned && temporary.path.str)
    (void)file_remove(&temporary);
  return ok;
}
