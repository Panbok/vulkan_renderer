#include "renderer/resources/loaders/cooked_font_loader.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_text.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/vkr_font_cooked.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_texture_system.h"

#include <math.h>

typedef struct VkrCookedFontRequest {
  String8 file_path;
  uint32_t size;
} VkrCookedFontRequest;

static String8 vkr_cooked_font_strip_query(String8 name) {
  for (uint64_t i = 0; i < name.length; ++i) {
    if (name.str[i] == '?') {
      return string8_substring(&name, 0, i);
    }
  }
  return name;
}

static VkrCookedFontRequest vkr_cooked_font_parse_request(String8 name) {
  String8 base_path = vkr_cooked_font_strip_query(name);
  VkrCookedFontRequest request = {
      .file_path = base_path,
      .size = VKR_MTSDF_FONT_DEFAULT_SIZE,
  };

  for (uint64_t start = base_path.length; start < name.length;) {
    ++start;
    uint64_t end = start;
    while (end < name.length && name.str[end] != '&') {
      ++end;
    }
    String8 param = string8_substring(&name, start, end);
    uint64_t equals = UINT64_MAX;
    for (uint64_t i = 0; i < param.length; ++i) {
      if (param.str[i] == '=') {
        equals = i;
        break;
      }
    }
    if (equals != UINT64_MAX && equals > 0 && equals + 1 < param.length) {
      String8 key = string8_substring(&param, 0, equals);
      String8 value = string8_substring(&param, equals + 1, param.length);
      String8 size_key = string8_lit("size");
      int32_t size = 0;
      if (string8_equalsi(&key, &size_key) && string8_to_i32(&value, &size) &&
          size > 0) {
        request.size = (uint32_t)size;
      }
    }
    start = end;
  }
  return request;
}

static bool8_t vkr_cooked_font_loader_can_load(VkrResourceLoader *self,
                                               String8 name) {
  (void)self;
  if (!name.str || name.length == 0) {
    return false_v;
  }
  String8 base_path = vkr_cooked_font_strip_query(name);
  const String8 extension = string8_lit("vkfa");
  for (uint64_t i = base_path.length; i > 0; --i) {
    if (base_path.str[i - 1] == '.') {
      String8 actual = string8_substring(&base_path, i, base_path.length);
      return string8_equalsi(&actual, &extension);
    }
  }
  return false_v;
}

static int32_t vkr_cooked_font_find_glyph(const VkrFontCookedDecoded *decoded,
                                          uint32_t glyph_id) {
  uint32_t lo = 0;
  uint32_t hi = decoded->glyph_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;
    uint32_t candidate = decoded->glyphs[mid].glyph_id;
    if (candidate < glyph_id) {
      lo = mid + 1u;
    } else if (candidate > glyph_id) {
      hi = mid;
    } else {
      return (int32_t)mid;
    }
  }
  return -1;
}

static int32_t vkr_cooked_font_clamp_i32(float32_t value, int32_t low,
                                         int32_t high) {
  if (value <= (float32_t)low) {
    return low;
  }
  if (value >= (float32_t)high) {
    return high;
  }
  return (int32_t)value;
}

static void vkr_cooked_font_copy_face(VkrFont *font, String8 face) {
  if (!font || !face.str || face.length == 0) {
    return;
  }
  uint64_t length = Min(face.length, sizeof(font->face) - 1u);
  MemCopy(font->face, face.str, length);
  font->face[length] = '\0';
}

static String8 vkr_cooked_font_atlas_name(VkrAllocator *allocator,
                                          const uint8_t identity[32]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t *storage =
      vkr_allocator_alloc(allocator, sizeof("cooked_mtsdf_atlas_") + 64u + 1u,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!storage) {
    return (String8){0};
  }
  const char prefix[] = "cooked_mtsdf_atlas_";
  const uint64_t prefix_length = sizeof(prefix) - 1u;
  MemCopy(storage, prefix, prefix_length);
  for (uint32_t i = 0; i < 32u; ++i) {
    storage[prefix_length + i * 2u] = (uint8_t)hex[identity[i] >> 4u];
    storage[prefix_length + i * 2u + 1u] = (uint8_t)hex[identity[i] & 0x0fu];
  }
  uint64_t length = prefix_length + 64u;
  storage[length] = '\0';
  return (String8){.str = storage, .length = length};
}

static bool8_t vkr_cooked_font_build_font(const VkrFontCookedDecoded *decoded,
                                          VkrAllocator *allocator,
                                          VkrTextureHandle atlas,
                                          uint32_t requested_size,
                                          VkrFont *out_font) {
  if (!decoded || !allocator || !out_font || decoded->glyph_count == 0 ||
      decoded->codepoint_count == 0 || decoded->page_count != 1u) {
    return false_v;
  }

  const VkrFontCookedPage *page = &decoded->pages[0];
  MemZero(out_font, sizeof(*out_font));
  out_font->type = VKR_FONT_TYPE_MTSDF;
  out_font->size = requested_size;
  out_font->atlas = atlas;
  out_font->page_count = 1u;
  out_font->atlas_size_x = (int32_t)page->width;
  out_font->atlas_size_y = (int32_t)page->height;
  out_font->fallback_glyph_id = decoded->fallback_glyph_id;
  out_font->em_line_height = decoded->metrics.line_height;
  out_font->em_ascender = decoded->metrics.ascender;
  out_font->em_descender = decoded->metrics.descender;
  out_font->em_underline_y = decoded->metrics.underline_y;
  out_font->em_underline_thickness = decoded->metrics.underline_thickness;
  out_font->sdf_distance_range = decoded->metrics.distance_range;
  out_font->mtsdf_unit_range = vkr_text_mtsdf_unit_range(
      decoded->metrics.distance_range, page->width, page->height);
  out_font->em_size = 1.0f;
  vkr_cooked_font_copy_face(out_font, decoded->face);

  out_font->glyphs_by_id =
      array_create_VkrFontGlyphId(allocator, decoded->glyph_count);
  out_font->codepoint_map = array_create_VkrFontCodepointMapEntry(
      allocator, decoded->codepoint_count);
  if (!out_font->glyphs_by_id.data || !out_font->codepoint_map.data) {
    return false_v;
  }

  for (uint32_t i = 0; i < decoded->glyph_count; ++i) {
    const VkrFontCookedGlyph *src = &decoded->glyphs[i];
    VkrFontGlyphId *dst = &out_font->glyphs_by_id.data[i];
    *dst = (VkrFontGlyphId){
        .glyph_id = src->glyph_id,
        .page_index = src->page_index,
        .has_geometry = (src->flags & VKR_FONT_COOKED_GLYPH_HAS_GEOMETRY) != 0,
        .advance = src->advance,
        .plane_left = src->plane_left,
        .plane_bottom = src->plane_bottom,
        .plane_right = src->plane_right,
        .plane_top = src->plane_top,
        .uv_left = src->uv_left,
        .uv_bottom = src->uv_bottom,
        .uv_right = src->uv_right,
        .uv_top = src->uv_top,
    };
  }

  const int32_t fallback_index =
      vkr_cooked_font_find_glyph(decoded, decoded->fallback_glyph_id);
  if (fallback_index < 0) {
    return false_v;
  }
  out_font->fallback_glyph_index = (uint32_t)fallback_index;

  for (uint32_t i = 0; i < decoded->codepoint_count; ++i) {
    const VkrFontCookedCodepoint *mapping = &decoded->codepoints[i];
    int32_t glyph_index =
        vkr_cooked_font_find_glyph(decoded, mapping->glyph_id);
    if (glyph_index < 0) {
      return false_v;
    }
    VkrFontCodepointMapEntry *map = &out_font->codepoint_map.data[i];
    map->codepoint = mapping->codepoint;
    map->glyph_id = mapping->glyph_id;
    map->glyph_index = (uint32_t)glyph_index;
  }

  if (decoded->kerning_count > 0) {
    out_font->glyph_kernings =
        array_create_VkrFontGlyphKerning(allocator, decoded->kerning_count);
    if (!out_font->glyph_kernings.data) {
      return false_v;
    }
    for (uint32_t i = 0; i < decoded->kerning_count; ++i) {
      out_font->glyph_kernings.data[i] = (VkrFontGlyphKerning){
          .left_glyph_id = decoded->kernings[i].left_glyph_id,
          .right_glyph_id = decoded->kernings[i].right_glyph_id,
          .amount = decoded->kernings[i].amount,
      };
    }
  }

  const float32_t scale = (float32_t)requested_size;
  out_font->line_height = vkr_cooked_font_clamp_i32(
      decoded->metrics.line_height * scale, 0, INT32_MAX);
  out_font->ascent = vkr_cooked_font_clamp_i32(
      decoded->metrics.ascender * scale, 0, INT32_MAX);
  out_font->descent = vkr_cooked_font_clamp_i32(
      -decoded->metrics.descender * scale, 0, INT32_MAX);
  out_font->baseline = out_font->ascent;
  for (uint32_t i = 0; i < decoded->codepoint_count; ++i) {
    if (decoded->codepoints[i].codepoint == 32u) {
      const uint32_t glyph_index = out_font->codepoint_map.data[i].glyph_index;
      out_font->tab_x_advance =
          out_font->glyphs_by_id.data[glyph_index].advance * 4.0f;
      break;
    }
  }
  if (out_font->tab_x_advance == 0.0f) {
    out_font->tab_x_advance = 2.0f;
  }
  out_font->atlas_pages = array_create_VkrTextureHandle(allocator, 1u);
  if (!out_font->atlas_pages.data) {
    return false_v;
  }
  out_font->atlas_pages.data[0] = atlas;
  return true_v;
}

static bool8_t vkr_cooked_font_loader_load(VkrResourceLoader *self,
                                           String8 name,
                                           VkrAllocator *temp_alloc,
                                           VkrResourceHandleInfo *out_handle,
                                           VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp allocator is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_handle = (VkrResourceHandleInfo){.type = VKR_RESOURCE_TYPE_UNKNOWN,
                                        .loader_id = VKR_INVALID_ID};
  *out_error = VKR_RENDERER_ERROR_NONE;
  VkrMtsdfFontLoaderContext *context =
      (VkrMtsdfFontLoaderContext *)self->resource_system;
  if (!context || !context->arena_pool || !context->texture_system) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrAllocatorScope temp_scope = vkr_allocator_begin_scope(temp_alloc);
  if (!vkr_allocator_scope_is_valid(&temp_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  void *pool_chunk = NULL;
  Arena *result_arena = NULL;
  VkrCookedFontLoaderResult *result = NULL;
  VkrTextureHandle atlas = VKR_TEXTURE_HANDLE_INVALID;
  String8 atlas_name = {0};
  bool8_t atlas_published = false_v;
  bool8_t success = false_v;

  if (!context->arena_pool->initialized ||
      !(pool_chunk = vkr_arena_pool_acquire(context->arena_pool))) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }
  result_arena =
      arena_create_from_buffer(pool_chunk, context->arena_pool->chunk_size);
  if (!result_arena) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }
  VkrAllocator result_allocator = {.ctx = result_arena};
  vkr_allocator_arena(&result_allocator);
  result =
      vkr_allocator_alloc(&result_allocator, sizeof(VkrCookedFontLoaderResult),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!result) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }
  MemZero(result, sizeof(*result));
  result->arena = result_arena;
  result->pool_chunk = pool_chunk;
  result->allocator = result_allocator;

  VkrCookedFontRequest request = vkr_cooked_font_parse_request(name);
  String8 file_path = string8_duplicate(temp_alloc, &request.file_path);
  if (!file_path.str) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    goto cleanup;
  }
  FilePath path = file_path_create((const char *)file_path.str, temp_alloc,
                                   FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);
  FileHandle file = {0};
  if (file_open(&path, mode, &file) != FILE_ERROR_NONE) {
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    goto cleanup;
  }
  uint8_t *file_data = NULL;
  uint64_t file_size = 0;
  FileError read_error =
      file_read_all(&file, temp_alloc, &file_data, &file_size);
  file_close(&file);
  if (read_error != FILE_ERROR_NONE || !file_data || file_size == 0) {
    *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    goto cleanup;
  }

  VkrFontCookedDecoded decoded = {0};
  if (!vkr_font_cooked_decode(temp_alloc, file_data, file_size, &decoded)) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto cleanup;
  }
  if (decoded.field_kind != VKR_FONT_COOKED_FIELD_MTSDF ||
      decoded.page_count != 1u ||
      decoded.pages[0].pixel_format != VKR_FONT_COOKED_PIXEL_RGBA8_UNORM) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto cleanup;
  }

  const VkrFontCookedPage *page = &decoded.pages[0];
  atlas_name = vkr_cooked_font_atlas_name(&result->allocator, decoded.identity);
  if (!atlas_name.str || page->pixel_size == 0 || !page->pixels ||
      page->width > VKR_TEXTURE_MAX_DIMENSION ||
      page->height > VKR_TEXTURE_MAX_DIMENSION) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    goto cleanup;
  }
  VkrTextureDescription description = {
      .width = page->width,
      .height = page->height,
      .channels = VKR_TEXTURE_RGBA_CHANNELS,
      .mip_levels = 1u,
      .array_layers = 1u,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_FONT,
      .type = VKR_TEXTURE_TYPE_2D,
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
      .generation = VKR_INVALID_ID,
  };
  VkrTextureUploadRegion region = {
      .width = page->width,
      .height = page->height,
      .depth = 1u,
      .byte_size = page->pixel_size,
  };
  VkrTexturePreparedLoad prepared = {
      .description = description,
      .upload_data = (uint8_t *)page->pixels,
      .upload_data_size = page->pixel_size,
      .upload_regions = &region,
      .upload_region_count = 1u,
      .upload_mip_levels = 1u,
      .upload_array_layers = 1u,
      .upload_is_compressed = false_v,
  };
  if (!vkr_texture_system_finalize_prepared_load(
          context->texture_system, atlas_name, &prepared, &atlas, out_error)) {
    goto cleanup;
  }
  atlas_published = true_v;
  /* Each font owns one texture-system reference, including a deduplicated
     publication, so two font names can safely share the same cooked page. */
  vkr_texture_system_add_ref_by_handle(context->texture_system, atlas);
  if (!vkr_cooked_font_build_font(&decoded, &result->allocator, atlas,
                                  request.size, &result->font)) {
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    goto cleanup;
  }
  result->atlas_texture_name = atlas_name;
  result->success = true_v;
  result->error = VKR_RENDERER_ERROR_NONE;
  out_handle->type = VKR_RESOURCE_TYPE_MTSDF_FONT;
  out_handle->loader_id = self->id;
  out_handle->as.custom = result;
  success = true_v;

cleanup:
  if (!success && atlas_published) {
    vkr_texture_system_release(context->texture_system, atlas_name);
  }
  if (!success && result_arena) {
    if (result) {
      vkr_allocator_release_global_accounting(&result->allocator);
    }
    arena_destroy(result_arena);
    result_arena = NULL;
  }
  if (!success && pool_chunk) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
  }
  vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  return success;
}

static void vkr_cooked_font_loader_unload(VkrResourceLoader *self,
                                          const VkrResourceHandleInfo *handle,
                                          String8 name) {
  (void)name;
  VkrMtsdfFontLoaderContext *context =
      self ? (VkrMtsdfFontLoaderContext *)self->resource_system : NULL;
  VkrCookedFontLoaderResult *result =
      handle ? (VkrCookedFontLoaderResult *)handle->as.custom : NULL;
  if (!result) {
    return;
  }
  if (context && context->texture_system && result->font.atlas.id != 0 &&
      result->font.atlas.id != VKR_INVALID_ID) {
    vkr_texture_system_release(context->texture_system,
                               result->atlas_texture_name);
  }
  if (result->font.glyphs_by_id.data) {
    array_destroy_VkrFontGlyphId(&result->font.glyphs_by_id);
  }
  if (result->font.codepoint_map.data) {
    array_destroy_VkrFontCodepointMapEntry(&result->font.codepoint_map);
  }
  if (result->font.glyph_kernings.data) {
    array_destroy_VkrFontGlyphKerning(&result->font.glyph_kernings);
  }
  if (result->font.atlas_pages.data) {
    array_destroy_VkrTextureHandle(&result->font.atlas_pages);
  }
  Arena *arena = result->arena;
  void *pool_chunk = result->pool_chunk;
  if (arena) {
    vkr_allocator_release_global_accounting(&result->allocator);
    arena_destroy(arena);
  }
  if (pool_chunk && context && context->arena_pool) {
    vkr_arena_pool_release(context->arena_pool, pool_chunk);
  }
}

static uint32_t
vkr_cooked_font_loader_batch_load(VkrResourceLoader *self, const String8 *paths,
                                  uint32_t count, VkrAllocator *temp_alloc,
                                  VkrResourceHandleInfo *out_handles,
                                  VkrRendererError *out_errors) {
  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; ++i) {
    out_handles[i].type = VKR_RESOURCE_TYPE_UNKNOWN;
    out_handles[i].loader_id = VKR_INVALID_ID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
    loaded += vkr_cooked_font_loader_load(self, paths[i], temp_alloc,
                                          &out_handles[i], &out_errors[i]);
  }
  return loaded;
}

VkrResourceLoader
vkr_cooked_font_loader_create(VkrMtsdfFontLoaderContext *context) {
  return (VkrResourceLoader){
      .type = VKR_RESOURCE_TYPE_MTSDF_FONT,
      .resource_system = context,
      .can_load = vkr_cooked_font_loader_can_load,
      .load = vkr_cooked_font_loader_load,
      .unload = vkr_cooked_font_loader_unload,
      .batch_load = vkr_cooked_font_loader_batch_load,
  };
}
