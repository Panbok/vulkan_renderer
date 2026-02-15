#include "renderer/resources/loaders/mtsdf_font_loader.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_json.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_system.h"

#include "core/vkr_text.h"
#include "renderer/vkr_renderer.h"

typedef struct VkrMtsdfFontRequest {
  String8 file_path;
  String8 query;
  String8 atlas_path;
  uint32_t size;
} VkrMtsdfFontRequest;

vkr_internal String8 vkr_mtsdf_font_strip_query(String8 name,
                                                String8 *out_query) {
  for (uint64_t i = 0; i < name.length; ++i) {
    if (name.str[i] == '?') {
      if (out_query) {
        *out_query = string8_substring(&name, i + 1, name.length);
      }
      return string8_substring(&name, 0, i);
    }
  }
  if (out_query) {
    *out_query = (String8){0};
  }
  return name;
}

vkr_internal VkrMtsdfFontRequest
vkr_mtsdf_font_parse_request(String8 name, VkrAllocator *temp_alloc) {
  String8 query = {0};
  String8 base_path = vkr_mtsdf_font_strip_query(name, &query);

  VkrMtsdfFontRequest request = {
      .file_path = base_path,
      .query = query,
      .atlas_path = (String8){0},
      .size = VKR_MTSDF_FONT_DEFAULT_SIZE,
  };

  if (!query.str || query.length == 0) {
    return request;
  }

  uint64_t start = 0;
  while (start < query.length) {
    uint64_t end = start;
    while (end < query.length && query.str[end] != '&') {
      end++;
    }

    String8 param = string8_substring(&query, start, end);
    uint64_t eq_pos = UINT64_MAX;
    for (uint64_t i = 0; i < param.length; ++i) {
      if (param.str[i] == '=') {
        eq_pos = i;
        break;
      }
    }

    if (eq_pos != UINT64_MAX && eq_pos > 0 && eq_pos + 1 < param.length) {
      String8 key = string8_substring(&param, 0, eq_pos);
      String8 value = string8_substring(&param, eq_pos + 1, param.length);
      String8 key_size = string8_lit("size");
      String8 key_atlas = string8_lit("atlas");

      if (string8_equalsi(&key, &key_size)) {
        int32_t parsed = 0;
        if (string8_to_i32(&value, &parsed) && parsed > 0) {
          request.size = (uint32_t)parsed;
        }
      } else if (string8_equalsi(&key, &key_atlas)) {
        request.atlas_path = string8_duplicate(temp_alloc, &value);
      }
    }

    start = end + 1;
  }

  return request;
}

vkr_internal void vkr_mtsdf_font_copy_face(VkrFont *font, String8 face_name) {
  if (!font || !face_name.str || face_name.length == 0) {
    return;
  }

  uint64_t copy_len = face_name.length;
  if (copy_len >= sizeof(font->face)) {
    copy_len = sizeof(font->face) - 1;
  }
  MemCopy(font->face, face_name.str, copy_len);
  font->face[copy_len] = '\0';
}

vkr_internal void vkr_mtsdf_font_load_atlas_cpu_data(String8 atlas_path,
                                                     VkrAllocator *temp_alloc,
                                                     VkrAllocator *result_alloc,
                                                     VkrFont *font) {
  if (!atlas_path.str || !temp_alloc || !result_alloc || !font) {
    return;
  }

  FilePath fp = file_path_create((const char *)atlas_path.str, temp_alloc,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_warn("MtsdfFontLoader: failed to open atlas '%s' for CPU copy",
             string8_cstr(&atlas_path));
    return;
  }

  uint8_t *file_data = NULL;
  uint64_t file_size = 0;
  ferr = file_read_all(&fh, temp_alloc, &file_data, &file_size);
  file_close(&fh);

  if (ferr != FILE_ERROR_NONE || !file_data || file_size == 0) {
    log_warn("MtsdfFontLoader: failed to read atlas '%s' for CPU copy",
             string8_cstr(&atlas_path));
    return;
  }

  stbi_set_flip_vertically_on_load_thread(0);

  int32_t width = 0;
  int32_t height = 0;
  int32_t channels = 0;
  uint8_t *pixels =
      stbi_load_from_memory(file_data, (int32_t)file_size, &width, &height,
                            &channels, VKR_TEXTURE_RGBA_CHANNELS);
  if (!pixels) {
    log_warn("MtsdfFontLoader: failed to decode atlas '%s' for CPU copy",
             string8_cstr(&atlas_path));
    return;
  }

  uint64_t size =
      (uint64_t)width * (uint64_t)height * (uint64_t)VKR_TEXTURE_RGBA_CHANNELS;
  uint8_t *copy =
      vkr_allocator_alloc(result_alloc, size, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!copy) {
    stbi_image_free(pixels);
    log_warn("MtsdfFontLoader: out of memory for CPU atlas copy");
    return;
  }

  MemCopy(copy, pixels, (size_t)size);
  stbi_image_free(pixels);

  font->atlas_cpu_data = copy;
  font->atlas_cpu_size = size;
  font->atlas_cpu_channels = VKR_TEXTURE_RGBA_CHANNELS;
}

vkr_internal bool8_t vkr_mtsdf_parse_atlas(VkrJsonReader *reader,
                                           VkrMtsdfFontMetadata *metadata) {
  assert_log(reader != NULL, "Reader is NULL");
  assert_log(metadata != NULL, "Metadata is NULL");

  reader->pos = 0;
  if (!vkr_json_find_field(reader, "atlas")) {
    log_error("MtsdfFontLoader: missing 'atlas' field");
    return false_v;
  }

  vkr_json_skip_to(reader, '{');
  uint64_t atlas_start = reader->pos;

  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "distanceRange")) {
    vkr_json_parse_float(reader, &metadata->distance_range);
  }

  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "size")) {
    vkr_json_parse_float(reader, &metadata->size);
    if (metadata->size <= 0.0f) {
      log_error("MtsdfFontLoader: invalid font size: %f", metadata->size);
      return false_v;
    }
  }

  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "width")) {
    int32_t w = 0;
    vkr_json_parse_int(reader, &w);
    if (w <= 0) {
      log_error("MtsdfFontLoader: invalid atlas width: %d", w);
      return false_v;
    }
    metadata->atlas_width = (uint32_t)w;
  }

  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "height")) {
    int32_t h = 0;
    vkr_json_parse_int(reader, &h);
    if (h <= 0) {
      log_error("MtsdfFontLoader: invalid atlas height: %d", h);
      return false_v;
    }
    metadata->atlas_height = (uint32_t)h;
  }

  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "emSize")) {
    vkr_json_parse_float(reader, &metadata->em_size);
    if (metadata->em_size <= 0.0f) {
      log_error("MtsdfFontLoader: invalid em size: %f", metadata->em_size);
      return false_v;
    }
  }

  reader->pos = atlas_start;
  if (vkr_json_find_field(reader, "yOrigin")) {
    String8 origin = {0};
    if (vkr_json_parse_string(reader, &origin)) {
      String8 bottom = string8_lit("bottom");
      metadata->y_origin_bottom = string8_equalsi(&origin, &bottom);
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_mtsdf_parse_metrics(VkrJsonReader *reader,
                                             VkrMtsdfFontMetadata *metadata) {
  assert_log(reader != NULL, "Reader is NULL");
  assert_log(metadata != NULL, "Metadata is NULL");

  reader->pos = 0;
  if (!vkr_json_find_field(reader, "metrics")) {
    log_error("MtsdfFontLoader: missing 'metrics' field");
    return false_v;
  }

  vkr_json_skip_to(reader, '{');
  uint64_t metrics_start = reader->pos;

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "lineHeight")) {
    vkr_json_parse_float(reader, &metadata->line_height);
  }

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "ascender")) {
    vkr_json_parse_float(reader, &metadata->ascender);
  }

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "descender")) {
    vkr_json_parse_float(reader, &metadata->descender);
  }

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "underlineY")) {
    vkr_json_parse_float(reader, &metadata->underline_y);
  }

  reader->pos = metrics_start;
  if (vkr_json_find_field(reader, "underlineThickness")) {
    vkr_json_parse_float(reader, &metadata->underline_thickness);
  }

  return true_v;
}

vkr_internal bool8_t vkr_mtsdf_parse_glyph_bounds(
    VkrJsonReader *reader, const char *bounds_name, float32_t *left,
    float32_t *bottom, float32_t *right, float32_t *top) {
  assert_log(reader != NULL, "Reader is NULL");
  assert_log(bounds_name != NULL, "Bounds name is NULL");
  assert_log(left != NULL, "Left is NULL");
  assert_log(bottom != NULL, "Bottom is NULL");
  assert_log(right != NULL, "Right is NULL");
  assert_log(top != NULL, "Top is NULL");

  uint64_t saved_pos = reader->pos;

  if (!vkr_json_find_field(reader, bounds_name)) {
    reader->pos = saved_pos;
    return false_v;
  }

  vkr_json_skip_to(reader, '{');
  uint64_t bounds_start = reader->pos;

  reader->pos = bounds_start;
  if (vkr_json_find_field(reader, "left")) {
    vkr_json_parse_float(reader, left);
  }

  reader->pos = bounds_start;
  if (vkr_json_find_field(reader, "bottom")) {
    vkr_json_parse_float(reader, bottom);
  }

  reader->pos = bounds_start;
  if (vkr_json_find_field(reader, "right")) {
    vkr_json_parse_float(reader, right);
  }

  reader->pos = bounds_start;
  if (vkr_json_find_field(reader, "top")) {
    vkr_json_parse_float(reader, top);
  }

  return true_v;
}

vkr_internal bool8_t vkr_mtsdf_parse_glyphs(VkrJsonReader *reader,
                                            Vector_VkrMtsdfGlyph *out_glyphs) {
  assert_log(reader != NULL, "Reader is NULL");
  assert_log(out_glyphs != NULL, "Out glyphs is NULL");

  reader->pos = 0;
  if (!vkr_json_find_array(reader, "glyphs")) {
    log_error("MtsdfFontLoader: missing 'glyphs' field");
    return false_v;
  }

  while (vkr_json_next_array_element(reader)) {
    VkrJsonReader glyph_reader = {0};
    if (!vkr_json_enter_object(reader, &glyph_reader)) {
      break;
    }

    VkrMtsdfGlyph glyph = {0};

    if (vkr_json_find_field(&glyph_reader, "unicode")) {
      int32_t unicode = 0;
      vkr_json_parse_int(&glyph_reader, &unicode);
      if (unicode < 0 || unicode > 0x10FFFF) {
        log_warn("MtsdfFontLoader: invalid unicode value: %d", unicode);
        continue;
      }
      glyph.unicode = (uint32_t)unicode;
    } else {
      continue;
    }

    glyph_reader.pos = 0;
    if (vkr_json_find_field(&glyph_reader, "advance")) {
      vkr_json_parse_float(&glyph_reader, &glyph.advance);
    }

    glyph_reader.pos = 0;
    if (vkr_mtsdf_parse_glyph_bounds(&glyph_reader, "planeBounds",
                                     &glyph.plane_left, &glyph.plane_bottom,
                                     &glyph.plane_right, &glyph.plane_top)) {
      glyph.has_geometry = true_v;
    }

    glyph_reader.pos = 0;
    vkr_mtsdf_parse_glyph_bounds(&glyph_reader, "atlasBounds",
                                 &glyph.atlas_left, &glyph.atlas_bottom,
                                 &glyph.atlas_right, &glyph.atlas_top);

    vector_push_VkrMtsdfGlyph(out_glyphs, glyph);
    if (out_glyphs->length >= VKR_MTSDF_FONT_MAX_GLYPHS) {
      log_error("MtsdfFontLoader: glyph limit exceeded");
      return false_v;
    }
  }

  return out_glyphs->length > 0;
}

vkr_internal bool8_t vkr_mtsdf_build_font(VkrMtsdfFontMetadata *metadata,
                                          VkrAllocator *allocator,
                                          VkrTextureHandle atlas,
                                          float32_t target_size,
                                          String8 face_name,
                                          VkrFont *out_font) {
  assert_log(metadata != NULL, "Metadata is NULL");
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(atlas.id != VKR_INVALID_ID, "Atlas is invalid");
  assert_log(target_size > 0.0f, "Target size is not positive");
  assert_log(out_font != NULL, "Out font is NULL");

  MemZero(out_font, sizeof(*out_font));

  out_font->type = VKR_FONT_TYPE_MTSDF;
  out_font->size = (uint32_t)target_size;
  out_font->atlas = atlas;
  out_font->page_count = 1;

  vkr_mtsdf_font_copy_face(out_font, face_name);

  float32_t scale = target_size / metadata->em_size;

  float32_t line_height = metadata->line_height * scale;
  float32_t ascent = metadata->ascender * scale;
  float32_t descent = -metadata->descender * scale;

  out_font->line_height = (int32_t)(line_height + 0.5f);
  out_font->ascent = (int32_t)(ascent + 0.5f);
  out_font->descent = (int32_t)(descent + 0.5f);
  out_font->baseline = out_font->ascent;
  out_font->atlas_size_x = (int32_t)metadata->atlas_width;
  out_font->atlas_size_y = (int32_t)metadata->atlas_height;

  out_font->glyphs =
      array_create_VkrFontGlyph(allocator, metadata->glyphs.length);
  if (!out_font->glyphs.data) {
    return false_v;
  }

  for (uint64_t i = 0; i < metadata->glyphs.length; i++) {
    VkrMtsdfGlyph *src = &metadata->glyphs.data[i];
    VkrFontGlyph *dst = &out_font->glyphs.data[i];

    dst->codepoint = src->unicode;
    float32_t x_advance = src->advance * scale;
    dst->x_advance = (int16_t)(x_advance + 0.5f);
    dst->page_id = 0;

    if (src->has_geometry) {
      float32_t atlas_left = src->atlas_left;
      float32_t atlas_right = src->atlas_right;
      float32_t atlas_top = src->atlas_top;
      float32_t atlas_bottom = src->atlas_bottom;

      float32_t min_y = Min(atlas_top, atlas_bottom);
      float32_t max_y = Max(atlas_top, atlas_bottom);
      float32_t height = max_y - min_y;

      float32_t y_top_left = metadata->y_origin_bottom
                                 ? (float32_t)metadata->atlas_height - max_y
                                 : min_y;

      dst->x = (uint16_t)atlas_left;
      dst->y = (uint16_t)y_top_left;
      dst->width = (uint16_t)(atlas_right - atlas_left);
      dst->height = (uint16_t)height;

      dst->x_offset = (int16_t)(src->plane_left * scale);
      float32_t y_offset = (-src->plane_top * scale) + ascent;
      dst->y_offset = (int16_t)(y_offset + (y_offset >= 0.0f ? 0.5f : -0.5f));
    } else {
      dst->x = dst->y = dst->width = dst->height = 0;
      dst->x_offset = dst->y_offset = 0;
    }
  }

  uint64_t glyph_count = out_font->glyphs.length;
  uint64_t table_capacity = glyph_count * 2;
  if (table_capacity < VKR_HASH_TABLE_INITIAL_CAPACITY) {
    table_capacity = VKR_HASH_TABLE_INITIAL_CAPACITY;
  }
  out_font->glyph_indices =
      vkr_hash_table_create_uint32_t(allocator, table_capacity);

  for (uint64_t i = 0; i < glyph_count; i++) {
    String8 key = string8_create_formatted(allocator, "%u",
                                           out_font->glyphs.data[i].codepoint);
    if (!vkr_hash_table_insert_uint32_t(&out_font->glyph_indices,
                                        string8_cstr(&key), (uint32_t)i)) {
      log_warn("MtsdfFontLoader: failed to index glyph %u",
               out_font->glyphs.data[i].codepoint);
    }
  }

  if (metadata->kernings.length > 0) {
    out_font->kernings =
        array_create_VkrFontKerning(allocator, metadata->kernings.length);
    if (!out_font->kernings.data) {
      return false_v;
    }
    MemCopy(out_font->kernings.data, metadata->kernings.data,
            metadata->kernings.length * sizeof(VkrFontKerning));
    qsort(out_font->kernings.data, (size_t)out_font->kernings.length,
          sizeof(VkrFontKerning), vkr_font_kerning_qsort_compare);
  }

  VkrFontGlyph *space = NULL;
  for (uint64_t i = 0; i < out_font->glyphs.length; i++) {
    if (out_font->glyphs.data[i].codepoint == 32) {
      space = &out_font->glyphs.data[i];
      break;
    }
  }
  if (space) {
    out_font->tab_x_advance = (float32_t)space->x_advance * 4.0f;
  } else {
    out_font->tab_x_advance = (float32_t)out_font->size * 2.0f;
  }

  out_font->atlas_pages = array_create_VkrTextureHandle(allocator, 1);
  if (out_font->atlas_pages.data) {
    out_font->atlas_pages.data[0] = atlas;
  }

  out_font->mtsdf_glyphs = metadata->glyphs;
  out_font->sdf_distance_range = metadata->distance_range;
  out_font->em_size = metadata->em_size;

  return true_v;
}

vkr_internal bool8_t vkr_mtsdf_font_loader_can_load(VkrResourceLoader *self,
                                                    String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  String8 query = {0};
  String8 base_path = vkr_mtsdf_font_strip_query(name, &query);

  for (uint64_t i = base_path.length; i > 0; --i) {
    if (base_path.str[i - 1] == '.') {
      String8 ext = string8_substring(&base_path, i, base_path.length);
      String8 json = string8_lit("json");
      return string8_equalsi(&ext, &json);
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_mtsdf_font_loader_load(
    VkrResourceLoader *self, String8 name, VkrAllocator *temp_alloc,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp alloc is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrMtsdfFontLoaderContext *context =
      (VkrMtsdfFontLoaderContext *)self->resource_system;
  assert_log(context != NULL, "Context is NULL");

  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(temp_alloc);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  void *pool_chunk = NULL;
  Arena *result_arena = NULL;
  if (context->arena_pool && context->arena_pool->initialized) {
    pool_chunk = vkr_arena_pool_acquire(context->arena_pool);
    if (!pool_chunk) {
      vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }
    result_arena =
        arena_create_from_buffer(pool_chunk, context->arena_pool->chunk_size);
  } else {
    log_fatal("MtsdfFontLoader: arena pool not initialized");
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrAllocator result_alloc = {.ctx = result_arena};
  vkr_allocator_arena(&result_alloc);

  VkrMtsdfFontLoaderResult *result =
      vkr_allocator_alloc(&result_alloc, sizeof(VkrMtsdfFontLoaderResult),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!result) {
    arena_destroy(result_arena);
    if (pool_chunk) {
      vkr_arena_pool_release(context->arena_pool, pool_chunk);
    }
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  MemZero(result, sizeof(*result));
  result->arena = result_arena;
  result->pool_chunk = pool_chunk;
  result->allocator = result_alloc;

  VkrMtsdfFontRequest request = vkr_mtsdf_font_parse_request(name, temp_alloc);
  if (!request.atlas_path.str || request.atlas_path.length == 0) {
    log_error("MtsdfFontLoader: missing atlas path in request");
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto fail;
  }

  String8 file_path_nt = string8_duplicate(temp_alloc, &request.file_path);

  FilePath fp = file_path_create((const char *)file_path_nt.str, temp_alloc,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_error("MtsdfFontLoader: failed to open '%.*s'",
              (int32_t)request.file_path.length, request.file_path.str);
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    goto fail;
  }

  uint8_t *json_data = NULL;
  uint64_t json_size = 0;
  ferr = file_read_all(&fh, temp_alloc, &json_data, &json_size);
  file_close(&fh);

  if (ferr != FILE_ERROR_NONE || !json_data) {
    log_error("MtsdfFontLoader: failed to read '%.*s'",
              (int32_t)request.file_path.length, request.file_path.str);
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    goto fail;
  }

  VkrJsonReader reader = {.data = json_data, .length = json_size, .pos = 0};

  VkrMtsdfFontMetadata metadata = {0};
  metadata.y_origin_bottom = true_v;

  if (!vkr_mtsdf_parse_atlas(&reader, &metadata)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto fail;
  }
  if (metadata.atlas_width == 0 || metadata.atlas_height == 0) {
    log_error("MtsdfFontLoader: invalid atlas dimensions");
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto fail;
  }

  if (!vkr_mtsdf_parse_metrics(&reader, &metadata)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto fail;
  }

  Vector_VkrMtsdfGlyph glyphs = vector_create_VkrMtsdfGlyph(temp_alloc);
  if (!vkr_mtsdf_parse_glyphs(&reader, &glyphs)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto fail;
  }

  metadata.glyphs =
      array_create_VkrMtsdfGlyph(&result->allocator, glyphs.length);
  if (!metadata.glyphs.data) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto fail;
  }
  MemCopy(metadata.glyphs.data, glyphs.data,
          glyphs.length * sizeof(VkrMtsdfGlyph));

  VkrTextureHandle atlas = VKR_TEXTURE_HANDLE_INVALID;
  VkrResourceHandleInfo texture_info = {0};
  VkrRendererError tex_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_sync(VKR_RESOURCE_TYPE_TEXTURE,
                                     request.atlas_path, temp_alloc,
                                     &texture_info, &tex_error)) {
    String8 err = vkr_renderer_get_error_string(tex_error);
    log_error("MtsdfFontLoader: failed to load atlas '%s': %s",
              string8_cstr(&request.atlas_path), string8_cstr(&err));
    *out_error = tex_error;
    goto fail;
  }
  atlas = texture_info.as.texture;

  if (atlas.id == 0 || atlas.id == VKR_INVALID_ID) {
    log_error("MtsdfFontLoader: invalid atlas handle");
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto fail_unload_atlas;
  }

  if (context->texture_system) {
    VkrRendererError sampler_err = vkr_texture_system_update_sampler(
        context->texture_system, atlas, VKR_FILTER_LINEAR, VKR_FILTER_LINEAR,
        VKR_MIP_FILTER_LINEAR, false_v, VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
        VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
        VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE);
    if (sampler_err != VKR_RENDERER_ERROR_NONE) {
      String8 err = vkr_renderer_get_error_string(sampler_err);
      log_warn("MtsdfFontLoader: failed to update atlas sampler '%s': %s",
               string8_cstr(&request.atlas_path), string8_cstr(&err));
    }
  }

  float32_t target_size = request.size > 0
                              ? (float32_t)request.size
                              : (float32_t)VKR_MTSDF_FONT_DEFAULT_SIZE;
  String8 face_name = string8_get_stem(temp_alloc, request.file_path);
  if (metadata.em_size <= 0.0f) {
    metadata.em_size = target_size;
  }

  if (!vkr_mtsdf_build_font(&metadata, &result->allocator, atlas, target_size,
                            face_name, &result->font)) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto fail_unload_atlas;
  }

  vkr_mtsdf_font_load_atlas_cpu_data(request.atlas_path, temp_alloc,
                                     &result->allocator, &result->font);

  result->metadata = metadata;
  result->atlas_texture_name =
      string8_duplicate(&result->allocator, &request.atlas_path);
  result->success = true_v;
  result->error = VKR_RENDERER_ERROR_NONE;

  out_handle->type = VKR_RESOURCE_TYPE_MTSDF_FONT;
  out_handle->loader_id = self->id;
  out_handle->as.custom = result;
  *out_error = VKR_RENDERER_ERROR_NONE;

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;

fail_unload_atlas: {
  VkrResourceHandleInfo atlas_info = {
      .type = VKR_RESOURCE_TYPE_TEXTURE,
      .loader_id = VKR_INVALID_ID,
      .as.texture = atlas,
  };
  vkr_resource_system_unload(&atlas_info, request.atlas_path);
}

fail:
  arena_destroy(result_arena);
  if (pool_chunk && context->arena_pool) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
  }
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return false_v;
}

vkr_internal void
vkr_mtsdf_font_loader_unload(VkrResourceLoader *self,
                             const VkrResourceHandleInfo *handle,
                             String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");

  VkrMtsdfFontLoaderContext *context =
      (VkrMtsdfFontLoaderContext *)self->resource_system;
  VkrMtsdfFontLoaderResult *result =
      (VkrMtsdfFontLoaderResult *)handle->as.custom;

  if (!result) {
    return;
  }

  VkrFont *font = &result->font;
  if (result->atlas_texture_name.str && font->atlas.id != 0 &&
      font->atlas.id != VKR_INVALID_ID) {
    VkrResourceHandleInfo atlas_info = {
        .type = VKR_RESOURCE_TYPE_TEXTURE,
        .loader_id = VKR_INVALID_ID,
        .as.texture = font->atlas,
    };
    vkr_resource_system_unload(&atlas_info, result->atlas_texture_name);
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

  if (font->mtsdf_glyphs.data) {
    array_destroy_VkrMtsdfGlyph(&font->mtsdf_glyphs);
  }

  if (font->atlas_pages.data) {
    array_destroy_VkrTextureHandle(&font->atlas_pages);
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

vkr_internal uint32_t vkr_mtsdf_font_loader_batch_load(
    VkrResourceLoader *self, const String8 *paths, uint32_t count,
    VkrAllocator *temp_alloc, VkrResourceHandleInfo *out_handles,
    VkrRendererError *out_errors) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(temp_alloc != NULL, "Temp alloc is NULL");
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

    if (vkr_mtsdf_font_loader_load(self, paths[i], temp_alloc, &out_handles[i],
                                   &out_errors[i])) {
      loaded++;
    }
  }

  return loaded;
}

VkrResourceLoader
vkr_mtsdf_font_loader_create(VkrMtsdfFontLoaderContext *context) {
  return (VkrResourceLoader){
      .type = VKR_RESOURCE_TYPE_MTSDF_FONT,
      .resource_system = context,
      .can_load = vkr_mtsdf_font_loader_can_load,
      .load = vkr_mtsdf_font_loader_load,
      .unload = vkr_mtsdf_font_loader_unload,
      .batch_load = vkr_mtsdf_font_loader_batch_load,
  };
}
