#include "renderer/resources/loaders/system_font_loader.h"

#include <stddef.h>

#include "containers/str.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_allocator.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_system.h"

typedef struct VkrSystemFontParseState {
  VkrAllocator *load_allocator;
  VkrAllocator *temp_allocator;

  stbtt_fontinfo font_info;
  uint8_t *font_data;
  uint64_t font_data_size;

  float32_t scale;
  int32_t ascent;
  int32_t descent;
  int32_t line_gap;
  int32_t line_height;

  uint32_t font_size;
  uint32_t atlas_width;
  uint32_t atlas_height;

  String8 face_name;

  Vector_VkrFontGlyph glyphs;
  Vector_VkrFontKerning kernings;
  uint8_t *atlas_bitmap;

  VkrRendererError *out_error;
} VkrSystemFontParseState;

typedef struct VkrSystemFontRequest {
  String8 file_path;
  String8 query;
  uint32_t size;
} VkrSystemFontRequest;

vkr_internal String8 vkr_system_font_strip_query(String8 name,
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

vkr_internal VkrSystemFontRequest vkr_system_font_parse_request(String8 name) {
  String8 query = {0};
  String8 base_path = vkr_system_font_strip_query(name, &query);

  uint32_t size = VKR_SYSTEM_FONT_DEFAULT_SIZE;
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
      if (string8_equalsi(&key, &key_size)) {
        int32_t parsed = 0;
        if (string8_to_i32(&value, &parsed) && parsed > 0) {
          size = (uint32_t)parsed;
          break;
        }
      }
    }

    start = end + 1;
  }

  return (VkrSystemFontRequest){
      .file_path = base_path,
      .query = query,
      .size = size,
  };
}

vkr_internal bool8_t vkr_system_font_read_file(VkrSystemFontParseState *state,
                                               String8 file_path) {
  assert_log(state != NULL, "State is NULL");

  if (!file_path.str || file_path.length == 0) {
    *state->out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  FilePath fp =
      file_path_create((const char *)file_path.str, state->temp_allocator,
                       FILE_PATH_TYPE_RELATIVE);

  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  FileError ferr = file_open(&fp, mode, &fh);
  if (ferr != FILE_ERROR_NONE) {
    log_error("SystemFontLoader: failed to open '%.*s'",
              (int32_t)file_path.length, file_path.str);
    *state->out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  ferr = file_read_all(&fh, state->temp_allocator, &state->font_data,
                       &state->font_data_size);
  file_close(&fh);

  if (ferr != FILE_ERROR_NONE || !state->font_data) {
    log_error("SystemFontLoader: failed to read '%.*s'",
              (int32_t)file_path.length, file_path.str);
    *state->out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  return true_v;
}

vkr_internal bool8_t
vkr_system_font_init_stbtt(VkrSystemFontParseState *state) {
  assert_log(state != NULL, "State is NULL");

  int32_t font_offset = stbtt_GetFontOffsetForIndex(state->font_data, 0);
  if (font_offset < 0) {
    log_error("SystemFontLoader: invalid font file or index");
    *state->out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  if (!stbtt_InitFont(&state->font_info, state->font_data, font_offset)) {
    log_error("SystemFontLoader: stbtt_InitFont failed");
    *state->out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  state->scale =
      stbtt_ScaleForPixelHeight(&state->font_info, (float32_t)state->font_size);

  int32_t ascent_unscaled = 0;
  int32_t descent_unscaled = 0;
  int32_t line_gap_unscaled = 0;
  stbtt_GetFontVMetrics(&state->font_info, &ascent_unscaled, &descent_unscaled,
                        &line_gap_unscaled);

  state->ascent = (int32_t)(ascent_unscaled * state->scale + 0.5f);
  int32_t descent_px = (int32_t)(descent_unscaled * state->scale - 0.5f);
  if (descent_px < 0) {
    descent_px = -descent_px;
  }

  state->descent = descent_px;
  state->line_gap = (int32_t)(line_gap_unscaled * state->scale + 0.5f);
  state->line_height = state->ascent + state->descent + state->line_gap;

  return true_v;
}

vkr_internal bool8_t
vkr_system_font_rasterize_glyphs(VkrSystemFontParseState *state) {
  assert_log(state != NULL, "State is NULL");

  uint64_t atlas_size = (uint64_t)state->atlas_width * state->atlas_height;
  state->atlas_bitmap = vkr_allocator_alloc(state->temp_allocator, atlas_size,
                                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!state->atlas_bitmap) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(state->atlas_bitmap, atlas_size);

  uint32_t cursor_x = VKR_SYSTEM_FONT_ATLAS_PADDING;
  uint32_t cursor_y = VKR_SYSTEM_FONT_ATLAS_PADDING;
  uint32_t row_height = 0;

  for (uint32_t cp = VKR_SYSTEM_FONT_FIRST_CODEPOINT;
       cp <= VKR_SYSTEM_FONT_LAST_CODEPOINT; ++cp) {
    int32_t glyph_index = stbtt_FindGlyphIndex(&state->font_info, (int32_t)cp);
    if (glyph_index == 0 && cp != ' ') {
      continue;
    }

    int32_t advance_width = 0;
    stbtt_GetGlyphHMetrics(&state->font_info, glyph_index, &advance_width,
                           NULL);

    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&state->font_info, glyph_index, state->scale,
                            state->scale, &x0, &y0, &x1, &y1);

    int32_t glyph_width = x1 - x0;
    int32_t glyph_height = y1 - y0;

    if (glyph_width > 0 && glyph_height > 0 &&
        ((uint32_t)glyph_width + VKR_SYSTEM_FONT_ATLAS_PADDING * 2 >
             state->atlas_width ||
         (uint32_t)glyph_height + VKR_SYSTEM_FONT_ATLAS_PADDING * 2 >
             state->atlas_height)) {
      log_error("SystemFontLoader: glyph %u too large for atlas", cp);
      *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }

    if (cursor_x + (uint32_t)glyph_width + VKR_SYSTEM_FONT_ATLAS_PADDING >
        state->atlas_width) {
      cursor_x = VKR_SYSTEM_FONT_ATLAS_PADDING;
      cursor_y += row_height + VKR_SYSTEM_FONT_ATLAS_PADDING;
      row_height = 0;
    }

    if (cursor_y + (uint32_t)glyph_height + VKR_SYSTEM_FONT_ATLAS_PADDING >
        state->atlas_height) {
      log_error("SystemFontLoader: atlas too small for font size %u",
                state->font_size);
      *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }

    if (glyph_width > 0 && glyph_height > 0) {
      uint8_t *dest = state->atlas_bitmap +
                      (size_t)cursor_y * state->atlas_width + cursor_x;
      stbtt_MakeGlyphBitmap(&state->font_info, dest, glyph_width, glyph_height,
                            (int)state->atlas_width, state->scale, state->scale,
                            glyph_index);
    }

    VkrFontGlyph glyph = {
        .codepoint = cp,
        .x = (uint16_t)cursor_x,
        .y = (uint16_t)cursor_y,
        .width = (uint16_t)glyph_width,
        .height = (uint16_t)glyph_height,
        .x_offset = (int16_t)x0,
        .y_offset = (int16_t)(y0 + state->ascent),
        .x_advance = (int16_t)(advance_width * state->scale + 0.5f),
        .page_id = 0,
    };
    vector_push_VkrFontGlyph(&state->glyphs, glyph);

    cursor_x += (uint32_t)glyph_width + VKR_SYSTEM_FONT_ATLAS_PADDING;
    if ((uint32_t)glyph_height > row_height) {
      row_height = (uint32_t)glyph_height;
    }
  }

  return true_v;
}

vkr_internal bool8_t vkr_system_font_create_atlas_texture(
    VkrSystemFontParseState *state, VkrTextureSystem *texture_system,
    VkrTextureHandle *out_handle, String8 *out_name) {
  assert_log(state != NULL, "State is NULL");
  assert_log(texture_system != NULL, "Texture system is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_name != NULL, "Out name is NULL");

  uint64_t rgba_size = (uint64_t)state->atlas_width * state->atlas_height * 4;
  uint8_t *rgba_data = vkr_allocator_alloc(state->temp_allocator, rgba_size,
                                           VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!rgba_data) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  for (uint32_t y = 0; y < state->atlas_height; ++y) {
    uint32_t src_row = y * state->atlas_width;
    uint32_t dst_row = (state->atlas_height - 1 - y) * state->atlas_width;
    for (uint32_t x = 0; x < state->atlas_width; ++x) {
      uint8_t alpha = state->atlas_bitmap[src_row + x];
      uint64_t dst_idx = ((uint64_t)dst_row + x) * 4;
      rgba_data[dst_idx + 0] = 255;
      rgba_data[dst_idx + 1] = 255;
      rgba_data[dst_idx + 2] = 255;
      rgba_data[dst_idx + 3] = alpha;
    }
  }

  VkrTexturePropertyFlags props = vkr_texture_property_flags_create();
  bitset8_set(&props, VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT);

  VkrTextureDescription desc = {
      .width = state->atlas_width,
      .height = state->atlas_height,
      .channels = 4,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .type = VKR_TEXTURE_TYPE_2D,
      .properties = props,
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };

  String8 face = state->face_name;
  if (!face.str || face.length == 0) {
    face = string8_lit("font");
  }

  String8 tex_name = string8_create_formatted(
      state->load_allocator, "system_font_atlas_%ux%u_%.*s_%u",
      state->atlas_width, state->atlas_height, (int32_t)face.length, face.str,
      state->font_size);

  VkrRendererError tex_error = VKR_RENDERER_ERROR_NONE;
  VkrTextureOpaqueHandle backend_handle = vkr_renderer_create_texture(
      texture_system->renderer, &desc, rgba_data, &tex_error);
  if (tex_error != VKR_RENDERER_ERROR_NONE || backend_handle == NULL) {
    log_error("SystemFontLoader: failed to create atlas texture");
    *state->out_error = tex_error;
    return false_v;
  }

  if (!vkr_texture_system_register_external(
          texture_system, tex_name, backend_handle, &desc, out_handle)) {
    log_error("SystemFontLoader: failed to register atlas texture '%s'",
              string8_cstr(&tex_name));
    vkr_renderer_destroy_texture(texture_system->renderer, backend_handle);
    *state->out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  *out_name = tex_name;
  return true_v;
}

vkr_internal bool8_t vkr_system_font_build_result(
    VkrSystemFontParseState *state, VkrTextureHandle atlas, VkrFont *out_font) {
  assert_log(state != NULL, "State is NULL");
  assert_log(out_font != NULL, "Out font is NULL");

  MemZero(out_font, sizeof(*out_font));

  out_font->type = VKR_FONT_TYPE_SYSTEM;
  out_font->size = state->font_size;
  out_font->line_height = state->line_height;
  out_font->baseline = state->ascent;
  out_font->ascent = state->ascent;
  out_font->descent = state->descent;
  out_font->atlas_size_x = (int32_t)state->atlas_width;
  out_font->atlas_size_y = (int32_t)state->atlas_height;
  out_font->page_count = 1;
  out_font->atlas = atlas;

  if (state->face_name.str && state->face_name.length > 0) {
    uint64_t copy_len = state->face_name.length;
    if (copy_len >= sizeof(out_font->face)) {
      copy_len = sizeof(out_font->face) - 1;
    }
    MemCopy(out_font->face, state->face_name.str, copy_len);
    out_font->face[copy_len] = '\0';
  }

  if (state->glyphs.length == 0) {
    log_error("SystemFontLoader: no glyphs rasterized");
    *state->out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  out_font->glyphs =
      array_create_VkrFontGlyph(state->load_allocator, state->glyphs.length);
  if (!out_font->glyphs.data) {
    *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
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
      log_warn("SystemFontLoader: failed to index glyph %u", glyph->codepoint);
    }
  }

  if (state->kernings.length > 0) {
    out_font->kernings = array_create_VkrFontKerning(state->load_allocator,
                                                     state->kernings.length);
    if (!out_font->kernings.data) {
      *state->out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
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
    out_font->tab_x_advance = (float32_t)out_font->size * 2.0f;
  }

  out_font->atlas_pages =
      array_create_VkrTextureHandle(state->load_allocator, 1);
  if (out_font->atlas_pages.data) {
    out_font->atlas_pages.data[0] = atlas;
  }

  uint64_t pixel_count =
      (uint64_t)state->atlas_width * (uint64_t)state->atlas_height;
  uint64_t rgba_size = pixel_count * VKR_TEXTURE_RGBA_CHANNELS;
  uint8_t *cpu_rgba = vkr_allocator_alloc(state->load_allocator, rgba_size,
                                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (cpu_rgba) {
    for (uint32_t y = 0; y < state->atlas_height; ++y) {
      uint32_t src_row = y * state->atlas_width;
      uint32_t dst_row = (state->atlas_height - 1 - y) * state->atlas_width;
      for (uint32_t x = 0; x < state->atlas_width; ++x) {
        uint8_t alpha = state->atlas_bitmap[src_row + x];
        uint64_t idx = ((uint64_t)dst_row + x) * VKR_TEXTURE_RGBA_CHANNELS;
        cpu_rgba[idx + 0] = 255;
        cpu_rgba[idx + 1] = 255;
        cpu_rgba[idx + 2] = 255;
        cpu_rgba[idx + 3] = alpha;
      }
    }
    out_font->atlas_cpu_data = cpu_rgba;
    out_font->atlas_cpu_size = rgba_size;
    out_font->atlas_cpu_channels = VKR_TEXTURE_RGBA_CHANNELS;
  } else {
    log_warn("SystemFontLoader: failed to allocate CPU atlas copy");
  }

  return true_v;
}

vkr_internal bool8_t vkr_system_font_remove_atlas_by_entry(
    VkrTextureSystem *system, const VkrTextureEntry *entry,
    const char *key_cstr) {
  assert_log(system != NULL, "System is NULL");
  assert_log(entry != NULL, "Entry is NULL");
  assert_log(key_cstr != NULL, "Key cstr is NULL");

  uint32_t texture_index = entry->index;
  if (texture_index >= system->textures.length) {
    return false_v;
  }

  if (system->default_texture.id > 0 &&
      texture_index == system->default_texture.id - 1) {
    log_warn("SystemFontLoader: refusing to remove default texture");
    return false_v;
  }

  VkrTexture *texture = &system->textures.data[texture_index];
  vkr_texture_destroy(system->renderer, texture);

  texture->description.id = VKR_INVALID_ID;
  texture->description.generation = VKR_INVALID_ID;

  vkr_hash_table_remove_VkrTextureEntry(&system->texture_map, key_cstr);

  if (texture_index < system->next_free_index) {
    system->next_free_index = texture_index;
  }

  return true_v;
}

vkr_internal void vkr_system_font_destroy_atlas_texture(
    VkrTextureSystem *system, String8 atlas_name, VkrTextureHandle atlas) {
  assert_log(system != NULL, "System is NULL");
  assert_log(atlas_name.str != NULL, "Atlas name is NULL");
  assert_log(atlas.id != 0 && atlas.id != VKR_INVALID_ID, "Atlas is invalid");

  const char *name_cstr = NULL;
  if (atlas_name.str && atlas_name.length > 0) {
    name_cstr = string8_cstr(&atlas_name);
  }

  if (name_cstr) {
    VkrTextureEntry *entry =
        vkr_hash_table_get_VkrTextureEntry(&system->texture_map, name_cstr);
    if (entry) {
      vkr_system_font_remove_atlas_by_entry(system, entry, name_cstr);
      return;
    }
  }

  for (uint64_t i = 0; i < system->texture_map.capacity; ++i) {
    VkrHashEntry_VkrTextureEntry *entry = &system->texture_map.entries[i];
    if (entry->occupied != VKR_OCCUPIED) {
      continue;
    }

    uint32_t texture_index = entry->value.index;
    if (texture_index >= system->textures.length) {
      continue;
    }

    VkrTexture *texture = &system->textures.data[texture_index];
    if (texture->description.id == atlas.id &&
        texture->description.generation == atlas.generation) {
      vkr_system_font_remove_atlas_by_entry(system, &entry->value, entry->key);
      return;
    }
  }

  log_warn("SystemFontLoader: atlas texture not found for cleanup");
}

vkr_internal bool8_t vkr_system_font_loader_can_load(VkrResourceLoader *self,
                                                     String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  String8 base_path = vkr_system_font_strip_query(name, NULL);
  for (uint64_t i = base_path.length; i > 0; --i) {
    if (base_path.str[i - 1] == '.') {
      String8 ext = string8_substring(&base_path, i, base_path.length);
      String8 ttf = string8_lit("ttf");
      String8 otf = string8_lit("otf");
      return string8_equalsi(&ext, &ttf) || string8_equalsi(&ext, &otf);
    }
  }
  return false_v;
}

vkr_internal bool8_t vkr_system_font_loader_load(
    VkrResourceLoader *self, String8 name, VkrAllocator *temp_alloc,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp alloc is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrSystemFontLoaderContext *context =
      (VkrSystemFontLoaderContext *)self->resource_system;
  assert_log(context != NULL, "Context is NULL");
  assert_log(context->texture_system != NULL, "Texture system is NULL");

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
    log_fatal("SystemFontLoader: arena pool not initialized");
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrAllocator result_alloc = {.ctx = result_arena};
  vkr_allocator_arena(&result_alloc);

  VkrSystemFontLoaderResult *result =
      vkr_allocator_alloc(&result_alloc, sizeof(VkrSystemFontLoaderResult),
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

  VkrSystemFontRequest request = vkr_system_font_parse_request(name);

  VkrSystemFontParseState state = {
      .load_allocator = &result->allocator,
      .temp_allocator = temp_alloc,
      .font_size = Clamp(request.size, VKR_SYSTEM_FONT_MIN_SIZE,
                         VKR_SYSTEM_FONT_MAX_SIZE),
      .atlas_width = VKR_SYSTEM_FONT_DEFAULT_ATLAS_SIZE,
      .atlas_height = VKR_SYSTEM_FONT_DEFAULT_ATLAS_SIZE,
      .out_error = out_error,
  };

  state.glyphs = vector_create_VkrFontGlyph(temp_alloc);
  state.kernings = vector_create_VkrFontKerning(temp_alloc);

  state.face_name = string8_get_stem(temp_alloc, request.file_path);

  String8 file_path_nt = string8_duplicate(temp_alloc, &request.file_path);

  if (!vkr_system_font_read_file(&state, file_path_nt)) {
    goto fail;
  }

  if (!vkr_system_font_init_stbtt(&state)) {
    goto fail;
  }

  if (!vkr_system_font_rasterize_glyphs(&state)) {
    goto fail;
  }

  for (uint64_t i = 0; i < state.glyphs.length; ++i) {
    for (uint64_t j = 0; j < state.glyphs.length; ++j) {
      uint32_t cp1 = state.glyphs.data[i].codepoint;
      uint32_t cp2 = state.glyphs.data[j].codepoint;

      int32_t kern = stbtt_GetCodepointKernAdvance(&state.font_info,
                                                   (int32_t)cp1, (int32_t)cp2);
      if (kern != 0) {
        VkrFontKerning kerning = {
            .codepoint_0 = cp1,
            .codepoint_1 = cp2,
            .amount = (int16_t)(kern * state.scale + 0.5f),
        };
        vector_push_VkrFontKerning(&state.kernings, kerning);
      }
    }
  }

  VkrTextureHandle atlas = VKR_TEXTURE_HANDLE_INVALID;
  String8 atlas_name = {0};
  if (!vkr_system_font_create_atlas_texture(&state, context->texture_system,
                                            &atlas, &atlas_name)) {
    goto fail;
  }

  if (!vkr_system_font_build_result(&state, atlas, &result->font)) {
    vkr_system_font_destroy_atlas_texture(context->texture_system, atlas_name,
                                          atlas);
    goto fail;
  }

  result->atlas_texture_name = atlas_name;
  result->success = true_v;
  result->error = VKR_RENDERER_ERROR_NONE;

  out_handle->type = VKR_RESOURCE_TYPE_SYSTEM_FONT;
  out_handle->loader_id = self->id;
  out_handle->as.custom = result;
  *out_error = VKR_RENDERER_ERROR_NONE;

  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return true_v;

fail:
  arena_destroy(result_arena);
  if (pool_chunk && context->arena_pool) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
  }
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return false_v;
}

vkr_internal void
vkr_system_font_loader_unload(VkrResourceLoader *self,
                              const VkrResourceHandleInfo *handle,
                              String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");

  VkrSystemFontLoaderContext *context =
      (VkrSystemFontLoaderContext *)self->resource_system;
  VkrSystemFontLoaderResult *result =
      (VkrSystemFontLoaderResult *)handle->as.custom;

  if (!result) {
    return;
  }

  VkrFont *font = &result->font;

  if (context && context->texture_system) {
    vkr_system_font_destroy_atlas_texture(
        context->texture_system, result->atlas_texture_name, font->atlas);
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

  void *pool_chunk = result->pool_chunk;
  Arena *arena = result->arena;

  if (arena) {
    arena_destroy(arena);
  }
  if (pool_chunk && context && context->arena_pool) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
  }
}

vkr_internal uint32_t vkr_system_font_loader_batch_load(
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

    if (vkr_system_font_loader_load(self, paths[i], temp_alloc, &out_handles[i],
                                    &out_errors[i])) {
      loaded++;
    }
  }

  return loaded;
}

VkrResourceLoader
vkr_system_font_loader_create(VkrSystemFontLoaderContext *context) {
  return (VkrResourceLoader){
      .type = VKR_RESOURCE_TYPE_SYSTEM_FONT,
      .resource_system = context,
      .can_load = vkr_system_font_loader_can_load,
      .load = vkr_system_font_loader_load,
      .unload = vkr_system_font_loader_unload,
      .batch_load = vkr_system_font_loader_batch_load,
  };
}
