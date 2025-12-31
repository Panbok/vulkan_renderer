#include "renderer/resources/loaders/bitmap_font_loader.h"

#include "containers/str.h"
#include "containers/vector.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_arena_pool.h"
#include "renderer/systems/vkr_resource_system.h"

#define VKR_FONT_CACHE_MAGIC 0x564B4654u /* 'VKFT' */
#define VKR_FONT_CACHE_VERSION 1u
#define VKR_FONT_CACHE_EXT ".vkf"
#define VKR_FONT_CACHE_MAX_FACE_LENGTH 1024u

typedef enum VkrBitmapFontFileType {
  VKR_BITMAP_FONT_FILE_TYPE_NOT_FOUND,
  VKR_BITMAP_FONT_FILE_TYPE_VKF,
  VKR_BITMAP_FONT_FILE_TYPE_FNT
} VkrBitmapFontFileType;

typedef struct VkrBitmapFontParseState {
  VkrAllocator *load_allocator;
  VkrAllocator *temp_allocator;

  String8 face_name;
  uint32_t font_size;
  bool8_t is_unicode;

  int32_t line_height;
  int32_t baseline;
  int32_t scale_w;
  int32_t scale_h;
  uint32_t page_count;

  Vector_VkrBitmapFontPage pages;
  Vector_VkrFontGlyph glyphs;
  Vector_VkrFontKerning kernings;

  uint8_t *atlas_cpu_data;
  uint64_t atlas_cpu_size;
  uint32_t atlas_cpu_channels;

  VkrRendererError *out_error;
} VkrBitmapFontParseState;

typedef struct VkrBitmapFontBinaryReader {
  uint8_t *ptr;
  uint8_t *end;
} VkrBitmapFontBinaryReader;

vkr_internal void vkr_bitmap_font_set_error(VkrBitmapFontParseState *state,
                                            VkrRendererError error);
vkr_internal void vkr_bitmap_font_reserve_pages(VkrBitmapFontParseState *state,
                                                uint32_t count);
vkr_internal void vkr_bitmap_font_reserve_glyphs(VkrBitmapFontParseState *state,
                                                 uint32_t count);
vkr_internal void
vkr_bitmap_font_reserve_kernings(VkrBitmapFontParseState *state,
                                 uint32_t count);

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
vkr_global const bool8_t vkr_bitmap_font_is_little_endian =
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
#else
vkr_global const union {
  uint32_t u32;
  uint8_t u8[4];
} vkr_bitmap_font_endian_check = {0x01020304};
vkr_global const bool8_t vkr_bitmap_font_is_little_endian =
    (vkr_bitmap_font_endian_check.u8[0] == 0x04);
#endif

vkr_internal uint32_t vkr_bitmap_font_host_to_little_u32(uint32_t value) {
  if (vkr_bitmap_font_is_little_endian) {
    return value;
  }

  return ((value & 0xFF000000) >> 24) | ((value & 0x00FF0000) >> 8) |
         ((value & 0x0000FF00) << 8) | ((value & 0x000000FF) << 24);
}

vkr_internal uint16_t vkr_bitmap_font_host_to_little_u16(uint16_t value) {
  if (vkr_bitmap_font_is_little_endian) {
    return value;
  }

  return (uint16_t)(((value & 0xFF00) >> 8) | ((value & 0x00FF) << 8));
}

vkr_internal bool8_t vkr_bitmap_font_cache_write_bytes(FileHandle *fh,
                                                       const void *data,
                                                       uint64_t size) {
  uint64_t written = 0;
  FileError err = file_write(fh, size, (const uint8_t *)data, &written);
  return err == FILE_ERROR_NONE && written == size;
}

vkr_internal bool8_t vkr_bitmap_font_cache_write_u32(FileHandle *fh,
                                                     uint32_t value) {
  uint32_t le_value = vkr_bitmap_font_host_to_little_u32(value);
  return vkr_bitmap_font_cache_write_bytes(fh, &le_value, sizeof(le_value));
}

vkr_internal bool8_t vkr_bitmap_font_cache_write_u16(FileHandle *fh,
                                                     uint16_t value) {
  uint16_t le_value = vkr_bitmap_font_host_to_little_u16(value);
  return vkr_bitmap_font_cache_write_bytes(fh, &le_value, sizeof(le_value));
}

vkr_internal bool8_t vkr_bitmap_font_cache_write_i32(FileHandle *fh,
                                                     int32_t value) {
  uint32_t u32_value = (uint32_t)value;
  return vkr_bitmap_font_cache_write_u32(fh, u32_value);
}

vkr_internal bool8_t vkr_bitmap_font_cache_write_i16(FileHandle *fh,
                                                     int16_t value) {
  uint16_t u16_value = (uint16_t)value;
  return vkr_bitmap_font_cache_write_u16(fh, u16_value);
}

vkr_internal bool8_t vkr_bitmap_font_cache_write_u8(FileHandle *fh,
                                                    uint8_t value) {
  return vkr_bitmap_font_cache_write_bytes(fh, &value, sizeof(value));
}

vkr_internal bool8_t vkr_bitmap_font_cache_read_bytes(
    VkrBitmapFontBinaryReader *reader, uint64_t size, void *out) {
  if (!reader || reader->ptr + size > reader->end) {
    return false_v;
  }
  if (out) {
    MemCopy(out, reader->ptr, size);
  }
  reader->ptr += size;
  return true_v;
}

vkr_internal bool8_t vkr_bitmap_font_cache_read_u32(
    VkrBitmapFontBinaryReader *reader, uint32_t *out) {
  uint32_t le_value = 0;
  if (!vkr_bitmap_font_cache_read_bytes(reader, sizeof(uint32_t), &le_value)) {
    return false_v;
  }
  if (out) {
    *out = vkr_bitmap_font_host_to_little_u32(le_value);
  }
  return true_v;
}

vkr_internal bool8_t vkr_bitmap_font_cache_read_u16(
    VkrBitmapFontBinaryReader *reader, uint16_t *out) {
  uint16_t le_value = 0;
  if (!vkr_bitmap_font_cache_read_bytes(reader, sizeof(uint16_t), &le_value)) {
    return false_v;
  }
  if (out) {
    *out = vkr_bitmap_font_host_to_little_u16(le_value);
  }
  return true_v;
}

vkr_internal bool8_t vkr_bitmap_font_cache_read_i32(
    VkrBitmapFontBinaryReader *reader, int32_t *out) {
  uint32_t u32_value = 0;
  if (!vkr_bitmap_font_cache_read_u32(reader, &u32_value)) {
    return false_v;
  }
  if (out) {
    *out = (int32_t)u32_value;
  }
  return true_v;
}

vkr_internal bool8_t vkr_bitmap_font_cache_read_i16(
    VkrBitmapFontBinaryReader *reader, int16_t *out) {
  uint16_t u16_value = 0;
  if (!vkr_bitmap_font_cache_read_u16(reader, &u16_value)) {
    return false_v;
  }
  if (out) {
    *out = (int16_t)u16_value;
  }
  return true_v;
}

vkr_internal bool8_t
vkr_bitmap_font_cache_read_u8(VkrBitmapFontBinaryReader *reader, uint8_t *out) {
  uint8_t value = 0;
  if (!vkr_bitmap_font_cache_read_bytes(reader, sizeof(uint8_t), &value)) {
    return false_v;
  }
  if (out) {
    *out = value;
  }
  return true_v;
}

vkr_internal String8 vkr_bitmap_font_cache_path(VkrAllocator *allocator,
                                                String8 source_path) {
  assert_log(allocator != NULL, "Allocator is NULL");
  return string8_create_formatted(allocator, "%.*s%s",
                                  (int32_t)source_path.length, source_path.str,
                                  VKR_FONT_CACHE_EXT);
}

vkr_internal bool8_t vkr_bitmap_font_cache_exists(VkrAllocator *allocator,
                                                  String8 cache_path) {
  if (!cache_path.str || cache_path.length == 0) {
    return false_v;
  }
  FilePath cache_fp = file_path_create((const char *)cache_path.str, allocator,
                                       FILE_PATH_TYPE_RELATIVE);
  return file_exists(&cache_fp);
}

vkr_internal bool8_t vkr_bitmap_font_cache_read(VkrBitmapFontParseState *state,
                                                String8 cache_path) {
  assert_log(state != NULL, "State is NULL");
  if (!cache_path.str || cache_path.length == 0) {
    return false_v;
  }

  String8 cache_path_nt = string8_duplicate(state->temp_allocator, &cache_path);
  FilePath cache_fp =
      file_path_create((const char *)cache_path_nt.str, state->temp_allocator,
                       FILE_PATH_TYPE_RELATIVE);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&cache_fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  uint8_t *file_data = NULL;
  uint64_t file_size = 0;
  FileError read_err =
      file_read_all(&fh, state->temp_allocator, &file_data, &file_size);
  file_close(&fh);
  if (read_err != FILE_ERROR_NONE || !file_data || file_size == 0) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  VkrBitmapFontBinaryReader reader = {
      .ptr = file_data,
      .end = file_data + file_size,
  };

  uint32_t magic = 0;
  uint32_t version = 0;
  if (!vkr_bitmap_font_cache_read_u32(&reader, &magic) ||
      !vkr_bitmap_font_cache_read_u32(&reader, &version)) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  if (magic != VKR_FONT_CACHE_MAGIC || version != VKR_FONT_CACHE_VERSION) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  uint32_t face_length = 0;
  uint32_t font_size = 0;
  int32_t line_height = 0;
  int32_t baseline = 0;
  int32_t atlas_size_x = 0;
  int32_t atlas_size_y = 0;
  uint32_t glyph_count = 0;
  uint32_t kerning_count = 0;
  uint32_t page_count = 0;

  if (!vkr_bitmap_font_cache_read_u32(&reader, &face_length) ||
      !vkr_bitmap_font_cache_read_u32(&reader, &font_size) ||
      !vkr_bitmap_font_cache_read_i32(&reader, &line_height) ||
      !vkr_bitmap_font_cache_read_i32(&reader, &baseline) ||
      !vkr_bitmap_font_cache_read_i32(&reader, &atlas_size_x) ||
      !vkr_bitmap_font_cache_read_i32(&reader, &atlas_size_y) ||
      !vkr_bitmap_font_cache_read_u32(&reader, &glyph_count) ||
      !vkr_bitmap_font_cache_read_u32(&reader, &kerning_count) ||
      !vkr_bitmap_font_cache_read_u32(&reader, &page_count)) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  if (face_length > VKR_FONT_CACHE_MAX_FACE_LENGTH ||
      reader.ptr + face_length > reader.end) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  if (face_length > 0) {
    String8 face_view = {.str = reader.ptr, .length = face_length};
    reader.ptr += face_length;
    state->face_name = string8_duplicate(state->load_allocator, &face_view);
  }

  state->font_size = font_size;
  state->line_height = line_height;
  state->baseline = baseline;
  state->scale_w = atlas_size_x;
  state->scale_h = atlas_size_y;
  state->page_count = page_count;

  if (page_count > 0) {
    vkr_bitmap_font_reserve_pages(state, page_count);
  }
  if (glyph_count > 0) {
    vkr_bitmap_font_reserve_glyphs(state, glyph_count);
  }
  if (kerning_count > 0) {
    vkr_bitmap_font_reserve_kernings(state, kerning_count);
  }

  for (uint32_t i = 0; i < page_count; ++i) {
    uint8_t page_id = 0;
    uint32_t file_length = 0;
    if (!vkr_bitmap_font_cache_read_u8(&reader, &page_id) ||
        !vkr_bitmap_font_cache_read_u32(&reader, &file_length)) {
      vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }
    if (file_length >= sizeof(((VkrBitmapFontPage *)0)->file) ||
        reader.ptr + file_length > reader.end) {
      vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }

    VkrBitmapFontPage page = {0};
    page.id = page_id;
    if (file_length > 0) {
      MemCopy(page.file, reader.ptr, file_length);
      reader.ptr += file_length;
      page.file[file_length] = '\0';
    }
    vector_push_VkrBitmapFontPage(&state->pages, page);
  }

  for (uint32_t i = 0; i < glyph_count; ++i) {
    VkrFontGlyph glyph = {0};
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    int16_t x_offset = 0;
    int16_t y_offset = 0;
    int16_t x_advance = 0;
    uint8_t page_id = 0;

    if (!vkr_bitmap_font_cache_read_u32(&reader, &glyph.codepoint) ||
        !vkr_bitmap_font_cache_read_u16(&reader, &x) ||
        !vkr_bitmap_font_cache_read_u16(&reader, &y) ||
        !vkr_bitmap_font_cache_read_u16(&reader, &width) ||
        !vkr_bitmap_font_cache_read_u16(&reader, &height) ||
        !vkr_bitmap_font_cache_read_i16(&reader, &x_offset) ||
        !vkr_bitmap_font_cache_read_i16(&reader, &y_offset) ||
        !vkr_bitmap_font_cache_read_i16(&reader, &x_advance) ||
        !vkr_bitmap_font_cache_read_u8(&reader, &page_id)) {
      vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }

    glyph.x = x;
    glyph.y = y;
    glyph.width = width;
    glyph.height = height;
    glyph.x_offset = x_offset;
    glyph.y_offset = y_offset;
    glyph.x_advance = x_advance;
    glyph.page_id = page_id;
    vector_push_VkrFontGlyph(&state->glyphs, glyph);
  }

  for (uint32_t i = 0; i < kerning_count; ++i) {
    VkrFontKerning kerning = {0};
    int16_t amount = 0;
    if (!vkr_bitmap_font_cache_read_u32(&reader, &kerning.codepoint_0) ||
        !vkr_bitmap_font_cache_read_u32(&reader, &kerning.codepoint_1) ||
        !vkr_bitmap_font_cache_read_i16(&reader, &amount)) {
      vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }
    kerning.amount = amount;
    vector_push_VkrFontKerning(&state->kernings, kerning);
  }

  if (!state->face_name.str || state->font_size == 0 ||
      state->line_height <= 0 || state->scale_w <= 0 || state->scale_h <= 0 ||
      state->pages.length == 0 || state->glyphs.length == 0) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  return true_v;
}

// todo: consider doing temp writes to a temp file and then renaming to the
// final path via atomic rename operation
vkr_internal bool8_t
vkr_bitmap_font_cache_write(VkrAllocator *allocator, String8 cache_path,
                            const VkrBitmapFontParseState *state) {
  assert_log(allocator != NULL, "Allocator is NULL");

  if (!cache_path.str || cache_path.length == 0 || !state) {
    return false_v;
  }

  String8 cache_dir = file_path_get_directory(allocator, cache_path);
  if (cache_dir.length > 0 && !file_ensure_directory(allocator, &cache_dir)) {
    log_warn("BitmapFontLoader: failed to ensure cache dir '%.*s'",
             (int32_t)cache_dir.length, cache_dir.str);
    return false_v;
  }

  FilePath cache_fp = file_path_create((const char *)cache_path.str, allocator,
                                       FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_WRITE);
  bitset8_set(&mode, FILE_MODE_TRUNCATE);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&cache_fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    return false_v;
  }

  uint32_t face_length =
      state->face_name.str ? (uint32_t)state->face_name.length : 0;
  if (state->face_name.length > VKR_FONT_CACHE_MAX_FACE_LENGTH) {
    file_close(&fh);
    return false_v;
  }
  uint32_t page_count = (uint32_t)state->pages.length;
  uint32_t glyph_count = (uint32_t)state->glyphs.length;
  uint32_t kerning_count = (uint32_t)state->kernings.length;

  bool8_t ok = true_v;
  ok = ok && vkr_bitmap_font_cache_write_u32(&fh, VKR_FONT_CACHE_MAGIC);
  ok = ok && vkr_bitmap_font_cache_write_u32(&fh, VKR_FONT_CACHE_VERSION);
  ok = ok && vkr_bitmap_font_cache_write_u32(&fh, face_length);
  ok = ok && vkr_bitmap_font_cache_write_u32(&fh, state->font_size);
  ok = ok && vkr_bitmap_font_cache_write_i32(&fh, state->line_height);
  ok = ok && vkr_bitmap_font_cache_write_i32(&fh, state->baseline);
  ok = ok && vkr_bitmap_font_cache_write_i32(&fh, state->scale_w);
  ok = ok && vkr_bitmap_font_cache_write_i32(&fh, state->scale_h);
  ok = ok && vkr_bitmap_font_cache_write_u32(&fh, glyph_count);
  ok = ok && vkr_bitmap_font_cache_write_u32(&fh, kerning_count);
  ok = ok && vkr_bitmap_font_cache_write_u32(&fh, page_count);

  if (ok && face_length > 0) {
    ok = ok && vkr_bitmap_font_cache_write_bytes(&fh, state->face_name.str,
                                                 face_length);
  }

  for (uint32_t i = 0; ok && i < page_count; ++i) {
    VkrBitmapFontPage *page = vector_get_VkrBitmapFontPage(
        (Vector_VkrBitmapFontPage *)&state->pages, i);
    uint64_t file_len =
        strnlen(page->file, sizeof(((VkrBitmapFontPage *)0)->file));
    if (file_len > UINT32_MAX) {
      ok = false_v;
      break;
    }
    ok = ok && vkr_bitmap_font_cache_write_u8(&fh, page->id);
    ok = ok && vkr_bitmap_font_cache_write_u32(&fh, (uint32_t)file_len);
    if (file_len > 0) {
      ok = ok && vkr_bitmap_font_cache_write_bytes(&fh, page->file,
                                                   (uint32_t)file_len);
    }
  }

  for (uint32_t i = 0; ok && i < glyph_count; ++i) {
    VkrFontGlyph *glyph =
        vector_get_VkrFontGlyph((Vector_VkrFontGlyph *)&state->glyphs, i);
    ok = ok && vkr_bitmap_font_cache_write_u32(&fh, glyph->codepoint);
    ok = ok && vkr_bitmap_font_cache_write_u16(&fh, glyph->x);
    ok = ok && vkr_bitmap_font_cache_write_u16(&fh, glyph->y);
    ok = ok && vkr_bitmap_font_cache_write_u16(&fh, glyph->width);
    ok = ok && vkr_bitmap_font_cache_write_u16(&fh, glyph->height);
    ok = ok && vkr_bitmap_font_cache_write_i16(&fh, glyph->x_offset);
    ok = ok && vkr_bitmap_font_cache_write_i16(&fh, glyph->y_offset);
    ok = ok && vkr_bitmap_font_cache_write_i16(&fh, glyph->x_advance);
    ok = ok && vkr_bitmap_font_cache_write_u8(&fh, glyph->page_id);
  }

  for (uint32_t i = 0; ok && i < kerning_count; ++i) {
    VkrFontKerning *kerning =
        vector_get_VkrFontKerning((Vector_VkrFontKerning *)&state->kernings, i);
    ok = ok && vkr_bitmap_font_cache_write_u32(&fh, kerning->codepoint_0);
    ok = ok && vkr_bitmap_font_cache_write_u32(&fh, kerning->codepoint_1);
    ok = ok && vkr_bitmap_font_cache_write_i16(&fh, kerning->amount);
  }

  file_close(&fh);
  return ok;
}

vkr_internal bool8_t vkr_bitmap_font_is_space(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

vkr_internal void vkr_bitmap_font_set_error(VkrBitmapFontParseState *state,
                                            VkrRendererError error) {
  assert_log(state != NULL, "State is NULL");
  assert_log(state->out_error != NULL, "Out error is NULL");
  *state->out_error = error;
}

vkr_internal VkrBitmapFontParseState vkr_bitmap_font_parse_state_create(
    VkrAllocator *load_allocator, VkrAllocator *temp_allocator,
    VkrRendererError *out_error) {
  assert_log(load_allocator != NULL, "Load allocator is NULL");
  assert_log(temp_allocator != NULL, "Temp allocator is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrBitmapFontParseState state = {
      .load_allocator = load_allocator,
      .temp_allocator = temp_allocator,
      .face_name = {0},
      .font_size = 0,
      .is_unicode = false_v,
      .line_height = 0,
      .baseline = 0,
      .scale_w = 0,
      .scale_h = 0,
      .page_count = 0,
      .pages = {0},
      .glyphs = {0},
      .kernings = {0},
      .atlas_cpu_data = NULL,
      .atlas_cpu_size = 0,
      .atlas_cpu_channels = 0,
      .out_error = out_error,
  };

  state.pages = vector_create_VkrBitmapFontPage(temp_allocator);
  state.glyphs = vector_create_VkrFontGlyph(temp_allocator);
  state.kernings = vector_create_VkrFontKerning(temp_allocator);

  return state;
}

vkr_internal bool8_t vkr_bitmap_font_parse_key_value(String8 line,
                                                     const char *key,
                                                     String8 *out_value) {
  assert_log(key != NULL, "Key is NULL");
  assert_log(out_value != NULL, "Out value is NULL");
  assert_log(line.str != NULL, "Line is NULL");

  if (line.length == 0) {
    return false_v;
  }

  uint64_t key_len = string_length(key);
  if (key_len == 0 || line.length < key_len + 1) {
    return false_v;
  }

  bool8_t in_quotes = false_v;
  for (uint64_t i = 0; i + key_len < line.length; ++i) {
    uint8_t c = line.str[i];
    if (c == '"') {
      in_quotes = !in_quotes;
      continue;
    }
    if (in_quotes) {
      continue;
    }

    if (i > 0 && !vkr_bitmap_font_is_space(line.str[i - 1])) {
      continue;
    }

    if (MemCompare(line.str + i, key, key_len) != 0) {
      continue;
    }

    uint64_t eq = i + key_len;
    if (eq >= line.length || line.str[eq] != '=') {
      continue;
    }

    uint64_t value_start = eq + 1;
    if (value_start >= line.length) {
      return false_v;
    }

    if (line.str[value_start] == '"') {
      value_start++;
      uint64_t value_end = value_start;
      while (value_end < line.length && line.str[value_end] != '"') {
        value_end++;
      }
      if (value_end >= line.length) {
        return false_v;
      }
      *out_value = string8_substring(&line, value_start, value_end);
      return true_v;
    }

    uint64_t value_end = value_start;
    while (value_end < line.length &&
           !vkr_bitmap_font_is_space(line.str[value_end])) {
      value_end++;
    }
    *out_value = string8_substring(&line, value_start, value_end);
    return true_v;
  }

  return false_v;
}

vkr_internal bool8_t vkr_bitmap_font_parse_int(String8 line, const char *key,
                                               int32_t *out_value) {
  String8 value = {0};
  if (!vkr_bitmap_font_parse_key_value(line, key, &value)) {
    return false_v;
  }
  return string8_to_i32(&value, out_value);
}

vkr_internal void vkr_bitmap_font_reserve_pages(VkrBitmapFontParseState *state,
                                                uint32_t count) {
  assert_log(state != NULL, "State is NULL");
  assert_log(count > 0, "Count is 0");
  if (state->pages.length == 0 && state->pages.capacity < count) {
    vector_destroy_VkrBitmapFontPage(&state->pages);
    state->pages = vector_create_VkrBitmapFontPage_with_capacity(
        state->temp_allocator, (uint64_t)count);
  }
}

vkr_internal void vkr_bitmap_font_reserve_glyphs(VkrBitmapFontParseState *state,
                                                 uint32_t count) {
  assert_log(state != NULL, "State is NULL");
  assert_log(count > 0, "Count is 0");
  if (state->glyphs.length == 0 && state->glyphs.capacity < count) {
    vector_destroy_VkrFontGlyph(&state->glyphs);
    state->glyphs = vector_create_VkrFontGlyph_with_capacity(
        state->temp_allocator, (uint64_t)count);
  }
}

vkr_internal void
vkr_bitmap_font_reserve_kernings(VkrBitmapFontParseState *state,
                                 uint32_t count) {
  assert_log(state != NULL, "State is NULL");
  assert_log(count > 0, "Count is 0");

  if (state->kernings.length == 0 && state->kernings.capacity < count) {
    vector_destroy_VkrFontKerning(&state->kernings);
    state->kernings = vector_create_VkrFontKerning_with_capacity(
        state->temp_allocator, (uint64_t)count);
  }
}

vkr_internal bool8_t vkr_bitmap_font_parse_info(VkrBitmapFontParseState *state,
                                                String8 line) {
  assert_log(state != NULL, "State is NULL");

  String8 face = {0};
  if (!vkr_bitmap_font_parse_key_value(line, "face", &face)) {
    log_error("BitmapFontLoader: missing face in info line");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  int32_t size = 0;
  if (!vkr_bitmap_font_parse_int(line, "size", &size) || size <= 0) {
    log_error("BitmapFontLoader: invalid size in info line");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  state->face_name = string8_duplicate(state->load_allocator, &face);
  state->font_size = (uint32_t)size;

  int32_t unicode = 0;
  if (vkr_bitmap_font_parse_int(line, "unicode", &unicode)) {
    state->is_unicode = unicode != 0 ? true_v : false_v;
  }

  return true_v;
}

vkr_internal bool8_t
vkr_bitmap_font_parse_common(VkrBitmapFontParseState *state, String8 line) {
  assert_log(state != NULL, "State is NULL");

  int32_t line_height = 0;
  int32_t baseline = 0;
  int32_t scale_w = 0;
  int32_t scale_h = 0;
  int32_t pages = 0;

  if (!vkr_bitmap_font_parse_int(line, "lineHeight", &line_height) ||
      !vkr_bitmap_font_parse_int(line, "base", &baseline) ||
      !vkr_bitmap_font_parse_int(line, "scaleW", &scale_w) ||
      !vkr_bitmap_font_parse_int(line, "scaleH", &scale_h) ||
      !vkr_bitmap_font_parse_int(line, "pages", &pages)) {
    log_error("BitmapFontLoader: malformed common line");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  state->line_height = line_height;
  state->baseline = baseline;
  state->scale_w = scale_w;
  state->scale_h = scale_h;
  state->page_count = pages > 0 ? (uint32_t)pages : 0;
  if (state->page_count > 0) {
    vkr_bitmap_font_reserve_pages(state, state->page_count);
  }

  return true_v;
}

vkr_internal bool8_t vkr_bitmap_font_parse_page(VkrBitmapFontParseState *state,
                                                String8 line) {
  assert_log(state != NULL, "State is NULL");

  int32_t id = 0;
  if (!vkr_bitmap_font_parse_int(line, "id", &id) || id < 0 || id > 255) {
    log_error("BitmapFontLoader: invalid page id");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  String8 file = {0};
  if (!vkr_bitmap_font_parse_key_value(line, "file", &file) ||
      file.length == 0) {
    log_error("BitmapFontLoader: missing page file");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  if (file.length >= sizeof(((VkrBitmapFontPage *)0)->file)) {
    log_error("BitmapFontLoader: page file name too long");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  VkrBitmapFontPage page = {0};
  page.id = (uint8_t)id;
  MemCopy(page.file, file.str, file.length);
  page.file[file.length] = '\0';
  vector_push_VkrBitmapFontPage(&state->pages, page);
  return true_v;
}

vkr_internal bool8_t vkr_bitmap_font_parse_char(VkrBitmapFontParseState *state,
                                                String8 line) {
  assert_log(state != NULL, "State is NULL");

  int32_t id = 0;
  int32_t x = 0;
  int32_t y = 0;
  int32_t width = 0;
  int32_t height = 0;
  int32_t x_offset = 0;
  int32_t y_offset = 0;
  int32_t x_advance = 0;
  int32_t page_id = 0;

  if (!vkr_bitmap_font_parse_int(line, "id", &id) ||
      !vkr_bitmap_font_parse_int(line, "x", &x) ||
      !vkr_bitmap_font_parse_int(line, "y", &y) ||
      !vkr_bitmap_font_parse_int(line, "width", &width) ||
      !vkr_bitmap_font_parse_int(line, "height", &height) ||
      !vkr_bitmap_font_parse_int(line, "xoffset", &x_offset) ||
      !vkr_bitmap_font_parse_int(line, "yoffset", &y_offset) ||
      !vkr_bitmap_font_parse_int(line, "xadvance", &x_advance) ||
      !vkr_bitmap_font_parse_int(line, "page", &page_id)) {
    log_error("BitmapFontLoader: malformed char line");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  VkrFontGlyph glyph = {0};
  glyph.codepoint = (uint32_t)id;
  glyph.x = (uint16_t)x;
  glyph.y = (uint16_t)y;
  glyph.width = (uint16_t)width;
  glyph.height = (uint16_t)height;
  glyph.x_offset = (int16_t)x_offset;
  glyph.y_offset = (int16_t)y_offset;
  glyph.x_advance = (int16_t)x_advance;
  glyph.page_id = (uint8_t)page_id;
  vector_push_VkrFontGlyph(&state->glyphs, glyph);
  return true_v;
}

vkr_internal bool8_t
vkr_bitmap_font_parse_kerning(VkrBitmapFontParseState *state, String8 line) {
  assert_log(state != NULL, "State is NULL");

  int32_t first = 0;
  int32_t second = 0;
  int32_t amount = 0;
  if (!vkr_bitmap_font_parse_int(line, "first", &first) ||
      !vkr_bitmap_font_parse_int(line, "second", &second) ||
      !vkr_bitmap_font_parse_int(line, "amount", &amount)) {
    log_error("BitmapFontLoader: malformed kerning line");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  VkrFontKerning kerning = {0};
  kerning.codepoint_0 = (uint32_t)first;
  kerning.codepoint_1 = (uint32_t)second;
  kerning.amount = (int16_t)amount;
  vector_push_VkrFontKerning(&state->kernings, kerning);
  return true_v;
}

vkr_internal VkrBitmapFontFileType
vkr_bitmap_font_detect_file_type(VkrAllocator *allocator, String8 path) {
  (void)allocator;

  if (!path.str || path.length == 0) {
    return VKR_BITMAP_FONT_FILE_TYPE_NOT_FOUND;
  }

  for (uint64_t i = path.length; i > 0; --i) {
    if (path.str[i - 1] == '.') {
      String8 ext = string8_substring(&path, i, path.length);
      if (vkr_string8_equals_cstr_i(&ext, "fnt")) {
        return VKR_BITMAP_FONT_FILE_TYPE_FNT;
      }
      if (vkr_string8_equals_cstr_i(&ext, "vkf")) {
        return VKR_BITMAP_FONT_FILE_TYPE_VKF;
      }
      break;
    }
  }

  return VKR_BITMAP_FONT_FILE_TYPE_NOT_FOUND;
}

vkr_internal bool8_t vkr_bitmap_font_parse_fnt(VkrBitmapFontParseState *state,
                                               String8 file_path) {
  assert_log(state != NULL, "State is NULL");
  assert_log(file_path.str != NULL, "File path is NULL");
  assert_log(file_path.length > 0, "File path is empty");

  FilePath path =
      file_path_create((const char *)file_path.str, state->temp_allocator,
                       FILE_PATH_TYPE_RELATIVE);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);

  FileHandle fh = {0};
  FileError ferr = file_open(&path, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_error("BitmapFontLoader: failed to open '%s'", path.path.str);
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  String8 file_str = {0};
  FileError read_err = file_read_string(&fh, state->temp_allocator, &file_str);
  file_close(&fh);
  if (read_err != FILE_ERROR_NONE) {
    log_error("BitmapFontLoader: failed to read '%s'", path.path.str);
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_FILE_NOT_FOUND);
    return false_v;
  }

  uint64_t offset = 0;
  while (offset < file_str.length) {
    uint64_t line_end = offset;
    while (line_end < file_str.length && file_str.str[line_end] != '\n' &&
           file_str.str[line_end] != '\r') {
      line_end++;
    }

    String8 line = string8_substring(&file_str, offset, line_end);
    offset = line_end;
    while (offset < file_str.length &&
           (file_str.str[offset] == '\n' || file_str.str[offset] == '\r')) {
      offset++;
    }

    string8_trim(&line);
    if (line.length == 0 || line.str[0] == '#') {
      continue;
    }

    if (vkr_string8_starts_with(&line, "info")) {
      if (!vkr_bitmap_font_parse_info(state, line)) {
        return false_v;
      }
    } else if (vkr_string8_starts_with(&line, "common")) {
      if (!vkr_bitmap_font_parse_common(state, line)) {
        return false_v;
      }
    } else if (vkr_string8_starts_with(&line, "page")) {
      if (!vkr_bitmap_font_parse_page(state, line)) {
        return false_v;
      }
    } else if (vkr_string8_starts_with(&line, "chars")) {
      int32_t count = 0;
      if (vkr_bitmap_font_parse_int(line, "count", &count) && count > 0) {
        vkr_bitmap_font_reserve_glyphs(state, (uint32_t)count);
      }
    } else if (vkr_string8_starts_with(&line, "char ")) {
      if (!vkr_bitmap_font_parse_char(state, line)) {
        return false_v;
      }
    } else if (vkr_string8_starts_with(&line, "kernings")) {
      int32_t count = 0;
      if (vkr_bitmap_font_parse_int(line, "count", &count) && count > 0) {
        vkr_bitmap_font_reserve_kernings(state, (uint32_t)count);
      }
    } else if (vkr_string8_starts_with(&line, "kerning")) {
      if (!vkr_bitmap_font_parse_kerning(state, line)) {
        return false_v;
      }
    }
  }

  if (!state->face_name.str || state->font_size == 0 ||
      state->line_height <= 0 || state->scale_w <= 0 || state->scale_h <= 0) {
    log_error("BitmapFontLoader: missing required font metadata");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  if (state->pages.length == 0) {
    log_error("BitmapFontLoader: no pages defined");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  if (state->glyphs.length == 0) {
    log_warn("BitmapFontLoader: no glyphs parsed");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  if (state->page_count > 0 && state->pages.length != state->page_count) {
    log_warn("BitmapFontLoader: page count mismatch (%u vs %llu)",
             state->page_count, (unsigned long long)state->pages.length);
  }

  return true_v;
}

vkr_internal void
vkr_bitmap_font_unload_pages(const Array_VkrBitmapFontPage *pages,
                             const Array_VkrTextureHandle *handles) {
  assert_log(pages != NULL, "Pages is NULL");
  assert_log(handles != NULL, "Handles is NULL");
  assert_log(pages->data != NULL, "Pages data is NULL");
  assert_log(handles->data != NULL, "Handles data is NULL");

  uint64_t count =
      pages->length < handles->length ? pages->length : handles->length;
  for (uint64_t i = 0; i < count; ++i) {
    const VkrBitmapFontPage *page = &pages->data[i];
    VkrTextureHandle handle = handles->data[i];
    if (page->file[0] == '\0' || handle.id == 0) {
      continue;
    }

    char path_buffer[512];
    int32_t written = snprintf(path_buffer, sizeof(path_buffer),
                               "assets/textures/%s", page->file);
    if (written <= 0 || written >= (int32_t)sizeof(path_buffer)) {
      log_warn("BitmapFontLoader: page path too long; skipping unload");
      continue;
    }

    String8 path = string8_create_from_cstr((const uint8_t *)path_buffer,
                                            (uint64_t)written);
    VkrResourceHandleInfo atlas_info = {
        .type = VKR_RESOURCE_TYPE_TEXTURE,
        .loader_id = VKR_INVALID_ID,
        .as.texture = handle,
    };
    vkr_resource_system_unload(&atlas_info, path);
  }
}

vkr_internal bool8_t vkr_bitmap_font_load_atlas(
    VkrBitmapFontParseState *state, VkrAllocator *temp_alloc,
    Array_VkrBitmapFontPage *out_pages, Array_VkrTextureHandle *out_atlases,
    VkrTextureHandle *out_atlas) {
  assert_log(state != NULL, "State is NULL");
  assert_log(temp_alloc != NULL, "Temp allocator is NULL");
  assert_log(out_pages != NULL, "Out pages is NULL");
  assert_log(out_atlases != NULL, "Out atlases is NULL");
  assert_log(out_atlas != NULL, "Out atlas is NULL");

  if (state->pages.length == 0) {
    log_error("BitmapFontLoader: no pages defined");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  uint32_t max_page_id = 0;
  for (uint64_t i = 0; i < state->pages.length; ++i) {
    VkrBitmapFontPage *page = vector_get_VkrBitmapFontPage(&state->pages, i);
    if (page && page->id > max_page_id) {
      max_page_id = page->id;
    }
  }

  uint32_t page_slots = max_page_id + 1;
  if (state->page_count > page_slots) {
    page_slots = state->page_count;
  }

  *out_pages =
      array_create_VkrBitmapFontPage(state->load_allocator, page_slots);
  if (!out_pages->data) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }
  MemZero(out_pages->data, page_slots * sizeof(VkrBitmapFontPage));

  *out_atlases =
      array_create_VkrTextureHandle(state->load_allocator, page_slots);
  if (!out_atlases->data) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }

  for (uint32_t i = 0; i < page_slots; ++i) {
    out_atlases->data[i] = VKR_TEXTURE_HANDLE_INVALID;
  }

  for (uint64_t i = 0; i < state->pages.length; ++i) {
    VkrBitmapFontPage *page = vector_get_VkrBitmapFontPage(&state->pages, i);
    if (!page || page->file[0] == '\0') {
      log_error("BitmapFontLoader: page file is empty");
      vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
      return false_v;
    }
    out_pages->data[page->id] = *page;
  }

  for (uint64_t i = 0; i < state->pages.length; ++i) {
    VkrBitmapFontPage *page = vector_get_VkrBitmapFontPage(&state->pages, i);
    if (!page) {
      continue;
    }

    String8 atlas_path =
        string8_create_formatted(temp_alloc, "assets/textures/%s", page->file);

    VkrResourceHandleInfo texture_info = {0};
    VkrRendererError tex_error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE, atlas_path,
                                  temp_alloc, &texture_info, &tex_error)) {
      String8 err = vkr_renderer_get_error_string(tex_error);
      log_error("BitmapFontLoader: failed to load atlas '%s': %s",
                string8_cstr(&atlas_path), string8_cstr(&err));
      vkr_bitmap_font_set_error(state, tex_error);
      vkr_bitmap_font_unload_pages(out_pages, out_atlases);
      return false_v;
    }

    out_atlases->data[page->id] = texture_info.as.texture;
  }

  VkrTextureHandle primary = VKR_TEXTURE_HANDLE_INVALID;
  uint32_t primary_page = 0;
  if (page_slots > 0) {
    primary = out_atlases->data[0];
  }
  if (primary.id == 0) {
    for (uint32_t i = 0; i < page_slots; ++i) {
      if (out_atlases->data[i].id != 0) {
        primary = out_atlases->data[i];
        primary_page = i;
        break;
      }
    }
    if (primary.id != 0 && primary_page != 0) {
      log_warn("BitmapFontLoader: missing page 0; using page %u", primary_page);
    }
  }

  if (primary.id == 0) {
    log_error("BitmapFontLoader: no atlas pages loaded");
    vkr_bitmap_font_set_error(state,
                              VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED);
    return false_v;
  }

  *out_atlas = primary;

  if (state->atlas_cpu_data != NULL) {
    log_warn("BitmapFontLoader: atlas CPU data is already loaded");
    return true_v;
  }

  const VkrBitmapFontPage *page = &out_pages->data[primary_page];
  if (page->file[0] == '\0') {
    log_warn("BitmapFontLoader: primary page file is empty");
    return true_v;
  }

  char path_buffer[512];
  int32_t written = snprintf(path_buffer, sizeof(path_buffer),
                             "assets/textures/%s", page->file);
  if (written <= 0 && written >= (int32_t)sizeof(path_buffer)) {
    log_warn("BitmapFontLoader: page path too long; skipping load");
    return true_v;
  }

  FilePath fp =
      file_path_create(path_buffer, temp_alloc, FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_warn("BitmapFontLoader: failed to open atlas '%s' for CPU copy",
             path_buffer);
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  uint8_t *file_data = NULL;
  uint64_t file_size = 0;
  ferr = file_read_all(&fh, temp_alloc, &file_data, &file_size);
  file_close(&fh);

  if (ferr != FILE_ERROR_NONE || !file_data || file_size == 0) {
    log_warn("BitmapFontLoader: failed to read atlas '%s' for CPU copy",
             path_buffer);
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  stbi_set_flip_vertically_on_load_thread(0);
  int32_t width = 0;
  int32_t height = 0;
  int32_t channels = 0;
  uint8_t *pixels =
      stbi_load_from_memory(file_data, (int32_t)file_size, &width, &height,
                            &channels, VKR_TEXTURE_RGBA_CHANNELS);
  if (!pixels) {
    log_warn("BitmapFontLoader: failed to decode atlas '%s' for CPU copy",
             path_buffer);
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  uint64_t size =
      (uint64_t)width * (uint64_t)height * VKR_TEXTURE_RGBA_CHANNELS;
  uint8_t *copy = vkr_allocator_alloc(state->load_allocator, size,
                                      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (copy) {
    MemCopy(copy, pixels, (size_t)size);
    state->atlas_cpu_data = copy;
    state->atlas_cpu_size = size;
    state->atlas_cpu_channels = VKR_TEXTURE_RGBA_CHANNELS;
  } else {
    log_warn("BitmapFontLoader: failed to allocate CPU atlas copy");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }

  stbi_image_free(pixels);
  return true_v;
}

vkr_internal bool8_t vkr_bitmap_font_build_result(
    VkrBitmapFontParseState *state, VkrTextureHandle atlas,
    const Array_VkrTextureHandle *atlas_pages, uint32_t page_count,
    VkrFont *out_font) {
  assert_log(state != NULL, "State is NULL");
  assert_log(atlas.id != VKR_INVALID_ID, "Atlas is invalid");
  assert_log(out_font != NULL, "Out font is NULL");

  MemZero(out_font, sizeof(*out_font));

  out_font->type = VKR_FONT_TYPE_BITMAP;
  out_font->size = state->font_size;
  out_font->line_height = state->line_height;
  out_font->baseline = state->baseline;
  out_font->ascent = state->baseline;
  out_font->descent = state->line_height - state->baseline;
  out_font->atlas_size_x = state->scale_w;
  out_font->atlas_size_y = state->scale_h;
  out_font->page_count = page_count;
  out_font->atlas = atlas;
  if (atlas_pages && atlas_pages->data) {
    out_font->atlas_pages = *atlas_pages;
  }

  out_font->atlas_cpu_data = state->atlas_cpu_data;
  out_font->atlas_cpu_size = state->atlas_cpu_size;
  out_font->atlas_cpu_channels = state->atlas_cpu_channels;

  if (state->face_name.str && state->face_name.length > 0) {
    uint64_t copy_len = state->face_name.length;
    if (copy_len >= sizeof(out_font->face)) {
      copy_len = sizeof(out_font->face) - 1;
    }
    MemCopy(out_font->face, state->face_name.str, copy_len);
    out_font->face[copy_len] = '\0';
  }

  if (state->glyphs.length == 0) {
    log_warn("BitmapFontLoader: no glyphs in font");
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_INVALID_PARAMETER);
    return false_v;
  }

  out_font->glyphs =
      array_create_VkrFontGlyph(state->load_allocator, state->glyphs.length);
  if (!out_font->glyphs.data) {
    vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
    return false_v;
  }
  MemCopy(out_font->glyphs.data, state->glyphs.data,
          state->glyphs.length * sizeof(VkrFontGlyph));

  uint64_t glyph_count = out_font->glyphs.length;
  uint64_t table_capacity = glyph_count * 2;
  if (table_capacity < VKR_HASH_TABLE_INITIAL_CAPACITY) {
    table_capacity = VKR_HASH_TABLE_INITIAL_CAPACITY;
  }
  out_font->glyph_indices =
      vkr_hash_table_create_uint32_t(state->load_allocator, table_capacity);
  for (uint64_t i = 0; i < glyph_count; ++i) {
    VkrFontGlyph *glyph = &out_font->glyphs.data[i];
    String8 key =
        string8_create_formatted(state->load_allocator, "%u", glyph->codepoint);
    if (!vkr_hash_table_insert_uint32_t(&out_font->glyph_indices,
                                        string8_cstr(&key), (uint32_t)i)) {
      log_warn("BitmapFontLoader: failed to index glyph %u", glyph->codepoint);
    }
  }

  if (state->kernings.length > 0) {
    out_font->kernings = array_create_VkrFontKerning(state->load_allocator,
                                                     state->kernings.length);
    if (!out_font->kernings.data) {
      vkr_bitmap_font_set_error(state, VKR_RENDERER_ERROR_OUT_OF_MEMORY);
      return false_v;
    }
    MemCopy(out_font->kernings.data, state->kernings.data,
            state->kernings.length * sizeof(VkrFontKerning));
  }

  VkrFontGlyph *space = NULL;
  for (uint64_t i = 0; i < out_font->glyphs.length; ++i) {
    if (out_font->glyphs.data[i].codepoint == 32) {
      space = &out_font->glyphs.data[i];
      break;
    }
  }
  if (space) {
    out_font->tab_x_advance = (float32_t)space->x_advance * 4.0f;
  } else {
    log_warn("BitmapFontLoader: missing space glyph; using default tab width");
    out_font->tab_x_advance = (float32_t)out_font->size * 2.0f;
  }

  return true_v;
}

vkr_internal bool8_t vkr_bitmap_font_loader_can_load(VkrResourceLoader *self,
                                                     String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  for (uint64_t i = name.length; i > 0; --i) {
    if (name.str[i - 1] == '.') {
      String8 ext = string8_substring(&name, i, name.length);
      String8 fnt = string8_lit("fnt");
      String8 vkf = string8_lit("vkf");
      return string8_equalsi(&ext, &fnt) || string8_equalsi(&ext, &vkf);
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_bitmap_font_loader_load(
    VkrResourceLoader *self, String8 name, VkrAllocator *temp_alloc,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp alloc is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrBitmapFontLoaderContext *context =
      (VkrBitmapFontLoaderContext *)self->resource_system;
  assert_log(context != NULL, "Context is NULL");

  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(temp_alloc);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrBitmapFontFileType file_type =
      vkr_bitmap_font_detect_file_type(temp_alloc, name);
  if (file_type == VKR_BITMAP_FONT_FILE_TYPE_NOT_FOUND) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  void *pool_chunk = NULL;
  Arena *result_arena = NULL;
  if (context->arena_pool && context->arena_pool->initialized) {
    pool_chunk = vkr_arena_pool_acquire(context->arena_pool);
    if (!pool_chunk) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      return false_v;
    }
    result_arena =
        arena_create_from_buffer(pool_chunk, context->arena_pool->chunk_size);
  } else {
    log_fatal("BitmapFontLoader: arena pool is not initialized");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  if (!result_arena) {
    if (pool_chunk && context->arena_pool) {
      vkr_arena_pool_release(context->arena_pool, pool_chunk);
    }
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  VkrAllocator result_alloc = {.ctx = result_arena};
  vkr_allocator_arena(&result_alloc);

  VkrBitmapFontLoaderResult *result =
      vkr_allocator_alloc(&result_alloc, sizeof(VkrBitmapFontLoaderResult),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!result) {
    arena_destroy(result_arena);
    if (pool_chunk && context->arena_pool) {
      vkr_arena_pool_release(context->arena_pool, pool_chunk);
    }
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  MemZero(result, sizeof(VkrBitmapFontLoaderResult));
  result->arena = result_arena;
  result->pool_chunk = pool_chunk;
  result->allocator = (VkrAllocator){.ctx = result_arena};
  vkr_allocator_arena(&result->allocator);

  VkrBitmapFontParseState state = vkr_bitmap_font_parse_state_create(
      &result->allocator, temp_alloc, out_error);
  bool8_t loaded_from_cache = false_v;
  String8 cache_path = {0};

  if (file_type == VKR_BITMAP_FONT_FILE_TYPE_VKF) {
    if (!vkr_bitmap_font_cache_read(&state, name)) {
      if (*out_error == VKR_RENDERER_ERROR_NONE) {
        *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      }
      arena_destroy(result_arena);
      if (pool_chunk && context->arena_pool) {
        vkr_arena_pool_release(context->arena_pool, pool_chunk);
      }
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      return false_v;
    }
    loaded_from_cache = true_v;
  } else if (file_type == VKR_BITMAP_FONT_FILE_TYPE_FNT) {
    cache_path = vkr_bitmap_font_cache_path(temp_alloc, name);
    if (vkr_bitmap_font_cache_exists(temp_alloc, cache_path)) {
      if (vkr_bitmap_font_cache_read(&state, cache_path)) {
        loaded_from_cache = true_v;
      } else {
        log_warn("BitmapFontLoader: failed to load cache '%s', regenerating",
                 string8_cstr(&cache_path));
        *out_error = VKR_RENDERER_ERROR_NONE;
        state = vkr_bitmap_font_parse_state_create(&result->allocator,
                                                   temp_alloc, out_error);
      }
    }

    if (!loaded_from_cache) {
      if (!vkr_bitmap_font_parse_fnt(&state, name)) {
        if (*out_error == VKR_RENDERER_ERROR_NONE) {
          *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
        }
        arena_destroy(result_arena);
        if (pool_chunk && context->arena_pool) {
          vkr_arena_pool_release(context->arena_pool, pool_chunk);
        }
        vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
        return false_v;
      }

      if (cache_path.str &&
          !vkr_bitmap_font_cache_write(temp_alloc, cache_path, &state)) {
        log_warn("BitmapFontLoader: failed to write cache '%s'",
                 string8_cstr(&cache_path));
      }
    }
  }

  Array_VkrBitmapFontPage pages = {0};
  Array_VkrTextureHandle atlas_pages = {0};
  VkrTextureHandle atlas = VKR_TEXTURE_HANDLE_INVALID;
  if (!vkr_bitmap_font_load_atlas(&state, temp_alloc, &pages, &atlas_pages,
                                  &atlas)) {
    arena_destroy(result_arena);
    if (pool_chunk && context->arena_pool) {
      vkr_arena_pool_release(context->arena_pool, pool_chunk);
    }
    if (*out_error == VKR_RENDERER_ERROR_NONE) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  uint32_t page_count = (uint32_t)state.pages.length;
  if (!vkr_bitmap_font_build_result(&state, atlas, &atlas_pages, page_count,
                                    &result->font)) {
    vkr_bitmap_font_unload_pages(&pages, &atlas_pages);
    arena_destroy(result_arena);
    if (pool_chunk && context->arena_pool) {
      vkr_arena_pool_release(context->arena_pool, pool_chunk);
    }
    if (*out_error == VKR_RENDERER_ERROR_NONE) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return false_v;
  }

  result->pages = pages;
  result->success = true_v;
  result->error = VKR_RENDERER_ERROR_NONE;

  out_handle->type = VKR_RESOURCE_TYPE_BITMAP_FONT;
  out_handle->loader_id = self->id;
  out_handle->as.custom = result;
  *out_error = VKR_RENDERER_ERROR_NONE;

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;
}

vkr_internal void
vkr_bitmap_font_loader_unload(VkrResourceLoader *self,
                              const VkrResourceHandleInfo *handle,
                              String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  (void)name;

  if (handle->type != VKR_RESOURCE_TYPE_BITMAP_FONT) {
    log_warn("BitmapFontLoader: attempted to unload non-font resource");
    return;
  }

  VkrBitmapFontLoaderContext *context =
      (VkrBitmapFontLoaderContext *)self->resource_system;
  VkrBitmapFontLoaderResult *result =
      (VkrBitmapFontLoaderResult *)handle->as.custom;

  if (!result) {
    return;
  }

  VkrFont *font = &result->font;
  if (result->pages.data && font->atlas_pages.data) {
    vkr_bitmap_font_unload_pages(&result->pages, &font->atlas_pages);
  }

  if (font->atlas_cpu_data) {
    vkr_allocator_free(&result->allocator, font->atlas_cpu_data,
                       font->atlas_cpu_size, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    font->atlas_cpu_data = NULL;
    font->atlas_cpu_size = 0;
    font->atlas_cpu_channels = 0;
  }

  if (font->glyph_indices.entries) {
    vkr_hash_table_destroy_uint32_t(&font->glyph_indices);
  }
  if (font->glyphs.data) {
    array_destroy_VkrFontGlyph(&font->glyphs);
  }
  if (font->kernings.data) {
    array_destroy_VkrFontKerning(&font->kernings);
  }
  if (font->atlas_pages.data) {
    array_destroy_VkrTextureHandle(&font->atlas_pages);
  }
  if (result->pages.data) {
    array_destroy_VkrBitmapFontPage(&result->pages);
  }

  void *pool_chunk = result->pool_chunk;
  Arena *arena = result->arena;

  if (arena) {
    arena_destroy(arena);
  }

  if (pool_chunk && context && context->arena_pool) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
  }
}

vkr_internal uint32_t vkr_bitmap_font_loader_batch_load(
    VkrResourceLoader *self, const String8 *paths, uint32_t count,
    VkrAllocator *temp_alloc, VkrResourceHandleInfo *out_handles,
    VkrRendererError *out_errors) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  if (count == 0) {
    return 0;
  }

  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; i++) {
    out_handles[i].type = VKR_RESOURCE_TYPE_UNKNOWN;
    out_handles[i].loader_id = VKR_INVALID_ID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;

    if (vkr_bitmap_font_loader_load(self, paths[i], temp_alloc, &out_handles[i],
                                    &out_errors[i])) {
      loaded++;
    }
  }

  return loaded;
}

VkrResourceLoader
vkr_bitmap_font_loader_create(const VkrBitmapFontLoaderContext *context) {
  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_BITMAP_FONT;
  loader.resource_system = context;
  loader.load = vkr_bitmap_font_loader_load;
  loader.unload = vkr_bitmap_font_loader_unload;
  loader.batch_load = vkr_bitmap_font_loader_batch_load;
  loader.can_load = vkr_bitmap_font_loader_can_load;
  return loader;
}
