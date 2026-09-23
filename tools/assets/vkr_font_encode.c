#include "assets/vkr_font_encode.h"

#include "core/vkr_byte_io.h"
#include "core/vkr_hash.h"

#include "containers/bitset.h"
#include "core/vkr_atomic.h"
#include "filesystem/filesystem.h"
#include "platform/vkr_platform.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

static VkrAtomicUint64 s_vkr_font_cooked_temporary_counter = 0u;

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
  if (!vkr_f32_is_binary32() || !info || !info->face.str ||
      info->face.length == 0u ||
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
    cursor = vkr_align_up_u64(cursor, VKR_FONT_COOKED_ALIGNMENT);
    offsets[i] = cursor;
    if (!vkr_checked_add_u64(cursor, section_sizes[i], &cursor)) {
      return false_v;
    }
  }
  const uint64_t file_size =
      vkr_align_up_u64(cursor, VKR_FONT_COOKED_ALIGNMENT);
  if (file_size > VKR_FONT_COOKED_MAX_FILE_SIZE || file_size > SIZE_MAX) {
    return false_v;
  }
  uint8_t *artifact = vkr_allocator_alloc(scratch_allocator, file_size,
                                          VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (!artifact) {
    return false_v;
  }
  MemZero(artifact, file_size);
  vkr_store_le_u32(artifact + 0u, VKR_FONT_COOKED_MAGIC);
  vkr_store_le_u32(artifact + 4u, VKR_FONT_COOKED_VERSION);
  vkr_store_le_u32(artifact + 8u, VKR_FONT_COOKED_ENDIAN_TAG);
  vkr_store_le_u32(artifact + 12u, VKR_FONT_COOKED_HEADER_SIZE);
  vkr_store_le_u32(artifact + 16u, 0u);
  vkr_store_le_u32(artifact + 20u, info->field_kind);
  vkr_store_le_u32(artifact + 24u, info->fallback_glyph_id);
  vkr_store_le_u32(artifact + 28u, info->cooker_version);
  vkr_store_le_u32(artifact + 32u, info->glyph_count);
  vkr_store_le_u32(artifact + 36u, info->codepoint_count);
  vkr_store_le_u32(artifact + 40u, info->kerning_count);
  vkr_store_le_u32(artifact + 44u, info->page_count);
  vkr_store_le_u64(artifact + 48u, file_size);
  vkr_store_le_u32(artifact + 56u, (uint32_t)info->face.length);
  MemCopy(artifact + 64u, info->identity, 32u);
  vkr_store_le_f32(artifact + 96u, info->metrics.line_height);
  vkr_store_le_f32(artifact + 100u, info->metrics.ascender);
  vkr_store_le_f32(artifact + 104u, info->metrics.descender);
  vkr_store_le_f32(artifact + 108u, info->metrics.underline_y);
  vkr_store_le_f32(artifact + 112u, info->metrics.underline_thickness);
  vkr_store_le_f32(artifact + 116u, info->metrics.distance_range);
  vkr_store_le_f32(artifact + 120u, info->metrics.atlas_px_per_em);
  vkr_store_le_u32(artifact + 124u, info->metrics.units_per_em);
  VkrByteWriter writer = {.data = artifact, .size = file_size, .offset = 0};
  for (uint32_t i = 0; i < VKR_FONT_COOKED_SECTION_COUNT; ++i) {
    uint8_t *entry = artifact + VKR_FONT_COOKED_DIRECTORY_OFFSET +
                     i * VKR_FONT_COOKED_SECTION_SIZE;
    vkr_store_le_u32(entry, i + 1u);
    vkr_store_le_u64(entry + 8u, offsets[i]);
    vkr_store_le_u64(entry + 16u, section_sizes[i]);
  }
  writer.offset = offsets[0];
  if (!vkr_byte_writer_bytes(&writer, info->face.str, info->face.length))
    return false_v;
  writer.offset = offsets[1];
  for (uint32_t i = 0; i < info->glyph_count; ++i) {
    const VkrFontCookedGlyph *g = &info->glyphs[i];
    if (!vkr_byte_writer_u32(&writer, g->glyph_id) ||
        !vkr_byte_writer_u32(&writer, g->page_index) ||
        !vkr_byte_writer_u32(&writer, g->flags) ||
        !vkr_byte_writer_f32(&writer, g->advance) ||
        !vkr_byte_writer_f32(&writer, g->plane_left) ||
        !vkr_byte_writer_f32(&writer, g->plane_bottom) ||
        !vkr_byte_writer_f32(&writer, g->plane_right) ||
        !vkr_byte_writer_f32(&writer, g->plane_top) ||
        !vkr_byte_writer_f32(&writer, g->uv_left) ||
        !vkr_byte_writer_f32(&writer, g->uv_bottom) ||
        !vkr_byte_writer_f32(&writer, g->uv_right) ||
        !vkr_byte_writer_f32(&writer, g->uv_top))
      return false_v;
  }
  writer.offset = offsets[2];
  for (uint32_t i = 0; i < info->codepoint_count; ++i) {
    if (!vkr_byte_writer_u32(&writer, info->codepoints[i].codepoint) ||
        !vkr_byte_writer_u32(&writer, info->codepoints[i].glyph_id))
      return false_v;
  }
  writer.offset = offsets[3];
  for (uint32_t i = 0; i < info->kerning_count; ++i) {
    const VkrFontCookedKerning *k = &info->kernings[i];
    if (!vkr_byte_writer_u32(&writer, k->left_glyph_id) ||
        !vkr_byte_writer_u32(&writer, k->right_glyph_id) ||
        !vkr_byte_writer_f32(&writer, k->amount) ||
        !vkr_byte_writer_u32(&writer, 0u))
      return false_v;
  }
  writer.offset = offsets[4];
  const VkrFontCookedPage *page = &info->pages[0];
  if (!vkr_byte_writer_u32(&writer, page->width) ||
      !vkr_byte_writer_u32(&writer, page->height) ||
      !vkr_byte_writer_u32(&writer, page->row_stride) ||
      !vkr_byte_writer_u32(&writer, page->pixel_format) ||
      !vkr_byte_writer_u64(&writer, offsets[5]) ||
      !vkr_byte_writer_u64(&writer, page->pixel_size) ||
      !vkr_byte_writer_u32(&writer,
                           vkr_crc32(page->pixels, page->pixel_size)) ||
      !vkr_byte_writer_u32(&writer, 0u) || !vkr_byte_writer_u32(&writer, 0u) ||
      !vkr_byte_writer_u32(&writer, 0u))
    return false_v;
  writer.offset = offsets[5];
  if (!vkr_byte_writer_bytes(&writer, page->pixels, page->pixel_size))
    return false_v;
  for (uint32_t i = 0; i < VKR_FONT_COOKED_SECTION_COUNT; ++i) {
    uint8_t *entry = artifact + VKR_FONT_COOKED_DIRECTORY_OFFSET +
                     i * VKR_FONT_COOKED_SECTION_SIZE;
    vkr_store_le_u32(entry + 24u,
                     vkr_crc32(artifact + offsets[i], section_sizes[i]));
  }
  const uint32_t directory_crc =
      vkr_crc32(artifact + VKR_FONT_COOKED_DIRECTORY_OFFSET,
                VKR_FONT_COOKED_DIRECTORY_SIZE);
  vkr_store_le_u32(artifact + VKR_FONT_COOKED_DIRECTORY_CRC_OFFSET,
                   directory_crc);
  const uint32_t payload_crc =
      vkr_crc32(artifact + VKR_FONT_COOKED_DATA_OFFSET,
                file_size - VKR_FONT_COOKED_DATA_OFFSET);
  vkr_store_le_u32(artifact + VKR_FONT_COOKED_PAYLOAD_CRC_OFFSET, payload_crc);
  vkr_store_le_u32(artifact + VKR_FONT_COOKED_HEADER_CRC_OFFSET, 0u);
  vkr_store_le_u32(artifact + VKR_FONT_COOKED_HEADER_CRC_OFFSET,
                   vkr_crc32(artifact, VKR_FONT_COOKED_HEADER_SIZE));
  *out_data = artifact;
  *out_size = file_size;
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
