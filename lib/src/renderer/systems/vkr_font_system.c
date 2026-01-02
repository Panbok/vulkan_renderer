#include "renderer/systems/vkr_font_system.h"

#include "containers/str.h"
#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/resources/loaders/bitmap_font_loader.h"
#include "renderer/resources/loaders/mtsdf_font_loader.h"
#include "renderer/resources/loaders/system_font_loader.h"
#include "renderer/systems/vkr_resource_system.h"

// =============================================================================
// Font Config Parser Constants
// =============================================================================

#define VKR_FONT_CONFIG_MAX_LINE_LENGTH 1024
#define VKR_FONT_CONFIG_MAX_KEY_LENGTH 64
#define VKR_FONT_CONFIG_MAX_VALUE_LENGTH 512

// =============================================================================
// Font Config Parser
// =============================================================================

/**
 * @brief Trims leading and trailing whitespace from a String8.
 */
vkr_internal String8 vkr_font_config_trim(VkrAllocator *allocator,
                                          const String8 *str) {
  assert_log(allocator != NULL, "Allocator is NULL");

  if (!str || !str->str || str->length == 0) {
    return (String8){0};
  }

  uint64_t start = 0;
  uint64_t end = str->length;

  while (start < end && (str->str[start] == ' ' || str->str[start] == '\t' ||
                         str->str[start] == '\r' || str->str[start] == '\n')) {
    start++;
  }

  while (end > start &&
         (str->str[end - 1] == ' ' || str->str[end - 1] == '\t' ||
          str->str[end - 1] == '\r' || str->str[end - 1] == '\n')) {
    end--;
  }

  if (start >= end) {
    return (String8){0};
  }

  uint64_t len = end - start;
  uint8_t *buf =
      vkr_allocator_alloc(allocator, len + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!buf) {
    return (String8){0};
  }

  MemCopy(buf, str->str + start, len);
  buf[len] = '\0';
  return (String8){.str = buf, .length = len};
}

/**
 * @brief Parses the font type from a string value.
 */
vkr_internal bool8_t vkr_font_config_parse_type(const String8 *value,
                                                VkrFontType *out_type) {
  if (!value || !value->str || !out_type) {
    return false_v;
  }

  String8 bitmap_str = string8_lit("bitmap");
  String8 system_str = string8_lit("system");
  String8 mtsdf_str = string8_lit("mtsdf");

  if (string8_equalsi(value, &bitmap_str)) {
    *out_type = VKR_FONT_TYPE_BITMAP;
    return true_v;
  }

  if (string8_equalsi(value, &system_str)) {
    *out_type = VKR_FONT_TYPE_SYSTEM;
    return true_v;
  }

  if (string8_equalsi(value, &mtsdf_str)) {
    *out_type = VKR_FONT_TYPE_MTSDF;
    return true_v;
  }

  return false_v;
}

/**
 * @brief Parses a .fontcfg file and returns the configuration.
 * @param fontcfg_path Path to the .fontcfg file.
 * @param allocator Allocator for persistent strings (file, atlas, faces).
 * @param scratch_alloc Allocator for temporary parsing operations.
 * @return Parsed font configuration (check is_valid field).
 */
vkr_internal VkrFontConfig vkr_font_config_parse(String8 fontcfg_path,
                                                 VkrAllocator *allocator,
                                                 VkrAllocator *scratch_alloc) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(scratch_alloc != NULL, "Scratch allocator is NULL");

  VkrFontConfig config = {0};
  config.is_valid = false_v;

  if (!fontcfg_path.str || fontcfg_path.length == 0) {
    log_error("Font config: invalid path");
    return config;
  }

  if (!allocator || !scratch_alloc) {
    log_error("Font config: invalid allocators");
    return config;
  }

  // Get the directory containing the .fontcfg file for relative path resolution
  String8 config_dir = file_path_get_directory(scratch_alloc, fontcfg_path);

  FilePath fp = file_path_create((const char *)fontcfg_path.str, scratch_alloc,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle handle = {0};
  FileError fe = file_open(&fp, mode, &handle);

  if (fe != FILE_ERROR_NONE) {
    log_error("Font config: failed to open '%.*s'",
              (int32_t)fontcfg_path.length, fontcfg_path.str);
    return config;
  }

  bool8_t has_file = false_v;
  bool8_t has_type = false_v;

  while (true) {
    VkrAllocatorScope line_scope = vkr_allocator_begin_scope(scratch_alloc);
    if (!vkr_allocator_scope_is_valid(&line_scope)) {
      file_close(&handle);
      log_error("Font config: failed to allocate line scope");
      return config;
    }

    String8 raw_line = {0};
    fe = file_read_line(&handle, scratch_alloc, scratch_alloc,
                        VKR_FONT_CONFIG_MAX_LINE_LENGTH, &raw_line);

    if (fe == FILE_ERROR_EOF) {
      vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      break;
    }

    if (fe != FILE_ERROR_NONE) {
      vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      file_close(&handle);
      log_error("Font config: failed to read line");
      return config;
    }

    String8 line = vkr_font_config_trim(scratch_alloc, &raw_line);

    if (line.length == 0 || line.str[0] == '#' || line.str[0] == ';') {
      vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      continue;
    }

    uint64_t eq_pos = 0;
    bool8_t found_eq = false_v;
    for (uint64_t i = 0; i < line.length; i++) {
      if (line.str[i] == '=') {
        eq_pos = i;
        found_eq = true_v;
        break;
      }
    }

    if (!found_eq) {
      log_warn("Font config: malformed line (no '='): %.*s",
               (int32_t)line.length, line.str);
      vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      continue;
    }

    String8 key_raw = {.str = line.str, .length = eq_pos};
    String8 value_raw = {.str = line.str + eq_pos + 1,
                         .length = line.length - eq_pos - 1};

    String8 key = vkr_font_config_trim(scratch_alloc, &key_raw);
    String8 value = vkr_font_config_trim(scratch_alloc, &value_raw);

    if (key.length == 0) {
      vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      continue;
    }

    String8 key_file = string8_lit("file");
    String8 key_atlas = string8_lit("atlas");
    String8 key_type = string8_lit("type");
    String8 key_face = string8_lit("face");
    String8 key_size = string8_lit("size");

    if (string8_equalsi(&key, &key_file)) {
      String8 resolved = file_path_join(allocator, config_dir, value);
      config.file = resolved;
      has_file = true_v;
    } else if (string8_equalsi(&key, &key_atlas)) {
      String8 resolved = file_path_join(allocator, config_dir, value);
      config.atlas = resolved;
    } else if (string8_equalsi(&key, &key_type)) {
      if (vkr_font_config_parse_type(&value, &config.type)) {
        has_type = true_v;
      } else {
        log_error("Font config: unknown type '%.*s'", (int32_t)value.length,
                  value.str);
        vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
        file_close(&handle);
        return config;
      }
    } else if (string8_equalsi(&key, &key_face)) {
      if (config.face_count < VKR_FONT_CONFIG_MAX_FACES) {
        uint8_t *face_buf = vkr_allocator_alloc(
            allocator, value.length + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
        if (face_buf) {
          MemCopy(face_buf, value.str, value.length);
          face_buf[value.length] = '\0';
          config.faces[config.face_count++] =
              (String8){.str = face_buf, .length = value.length};
        }
      } else {
        log_warn("Font config: max faces (%d) reached, ignoring '%.*s'",
                 VKR_FONT_CONFIG_MAX_FACES, (int32_t)value.length, value.str);
      }
    } else if (string8_equalsi(&key, &key_size)) {
      int32_t size_val = 0;
      if (string8_to_i32(&value, &size_val) && size_val > 0) {
        config.size = (uint32_t)size_val;
      }
    } else {
      log_warn("Font config: unknown key '%.*s'", (int32_t)key.length, key.str);
    }

    vkr_allocator_end_scope(&line_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  file_close(&handle);

  if (!has_file) {
    log_error("Font config: missing required 'file' field in '%.*s'",
              (int32_t)fontcfg_path.length, fontcfg_path.str);
    return config;
  }

  if (!has_type) {
    log_error("Font config: missing required 'type' field in '%.*s'",
              (int32_t)fontcfg_path.length, fontcfg_path.str);
    return config;
  }

  if (config.type == VKR_FONT_TYPE_MTSDF &&
      (!config.atlas.str || config.atlas.length == 0)) {
    log_error("Font config: 'atlas' required for mtsdf type in '%.*s'",
              (int32_t)fontcfg_path.length, fontcfg_path.str);
    return config;
  }

  config.is_valid = true_v;
  return config;
}

// =============================================================================
// Internal helpers
// =============================================================================

vkr_internal uint32_t vkr_font_system_find_free_slot(VkrFontSystem *system) {
  assert_log(system != NULL, "System is NULL");

  uint32_t max_fonts = (uint32_t)system->fonts.length;
  if (max_fonts == 0) {
    return VKR_INVALID_ID;
  }

  for (uint32_t font_id = system->next_free_index; font_id < max_fonts;
       font_id++) {
    VkrFont *font = &system->fonts.data[font_id];
    if (font->generation == VKR_INVALID_ID) {
      system->next_free_index = font_id + 1;
      return font_id;
    }
  }

  for (uint32_t font_id = 0; font_id < system->next_free_index; font_id++) {
    VkrFont *font = &system->fonts.data[font_id];
    if (font->generation == VKR_INVALID_ID) {
      system->next_free_index = font_id + 1;
      return font_id;
    }
  }

  return VKR_INVALID_ID;
}

vkr_internal uint32_t vkr_font_system_get_font_count_from_file(
    String8 file_path, VkrAllocator *temp_alloc) {
  if (!file_path.str || file_path.length == 0 || !temp_alloc) {
    return 0;
  }

  FilePath fp = file_path_create((const char *)file_path.str, temp_alloc,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  bitset8_set(&mode, FILE_MODE_BINARY);

  FileHandle fh = {0};
  if (file_open(&fp, mode, &fh) != FILE_ERROR_NONE) {
    return 0;
  }

  uint8_t *font_data = NULL;
  uint64_t font_data_size = 0;
  FileError ferr = file_read_all(&fh, temp_alloc, &font_data, &font_data_size);
  file_close(&fh);

  if (ferr != FILE_ERROR_NONE || !font_data || font_data_size == 0) {
    return 0;
  }

  int32_t num_fonts = stbtt_GetNumberOfFonts(font_data);
  return (num_fonts > 0) ? (uint32_t)num_fonts : 1;
}

vkr_internal bool8_t vkr_font_system_load_single_variant(
    VkrFontSystem *system, String8 register_name, const VkrFontConfig *config,
    uint32_t font_index, VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!register_name.str || register_name.length == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  const char *font_key = (const char *)register_name.str;
  VkrFontSystemEntry *existing =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);
  if (existing) {
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  uint32_t free_slot = vkr_font_system_find_free_slot(system);
  if (free_slot == VKR_INVALID_ID) {
    log_error("Font system is full (max=%llu)",
              (unsigned long long)system->fonts.length);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrAllocatorScope load_scope =
      vkr_allocator_begin_scope(&system->temp_allocator);
  if (!vkr_allocator_scope_is_valid(&load_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  uint32_t size =
      config->size > 0 ? config->size : VKR_SYSTEM_FONT_DEFAULT_SIZE;
  String8 load_name = string8_create_formatted(
      &system->temp_allocator, "%.*s?size=%u&index=%u",
      (int32_t)config->file.length, config->file.str, size, font_index);

  VkrResourceHandleInfo handle_info = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_SYSTEM_FONT, load_name,
                                &system->allocator, &handle_info,
                                &load_error)) {
    *out_error = (load_error != VKR_RENDERER_ERROR_NONE)
                     ? load_error
                     : VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }

  VkrSystemFontLoaderResult *result =
      (VkrSystemFontLoaderResult *)handle_info.as.custom;
  if (!result || !result->success) {
    vkr_resource_system_unload(&handle_info, load_name);
    *out_error =
        result ? result->error : VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }

  VkrFont *font = &system->fonts.data[free_slot];
  *font = result->font;

  font->id = free_slot + 1;
  font->generation = system->generation_counter++;

  char *stable_key =
      (char *)vkr_allocator_alloc(&system->allocator, register_name.length + 1,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    vkr_resource_system_unload(&handle_info, load_name);
    MemZero(font, sizeof(*font));
    font->id = VKR_INVALID_ID;
    font->generation = VKR_INVALID_ID;
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }
  MemCopy(stable_key, register_name.str, (size_t)register_name.length);
  stable_key[register_name.length] = '\0';

  VkrFontSystemEntry entry = {
      .index = free_slot,
      .ref_count = 0,
      .auto_release = true_v,
      .loader_id = handle_info.loader_id,
      .resource = handle_info.as.custom,
  };

  if (!vkr_hash_table_insert_VkrFontSystemEntry(&system->font_map, stable_key,
                                                entry)) {
    vkr_resource_system_unload(&handle_info, load_name);
    MemZero(font, sizeof(*font));
    font->id = VKR_INVALID_ID;
    font->generation = VKR_INVALID_ID;
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }

  vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal VkrFont *vkr_font_system_get_by_index(VkrFontSystem *system,
                                                   uint32_t index) {
  if (!system || index >= system->fonts.length) {
    return NULL;
  }
  return &system->fonts.data[index];
}

vkr_internal void vkr_font_system_unload_font(VkrFontSystem *system,
                                              VkrFontSystemEntry *entry,
                                              String8 name,
                                              bool8_t remove_entry) {
  assert_log(system != NULL, "System is NULL");
  assert_log(entry != NULL, "Entry is NULL");

  if (entry->index == VKR_INVALID_ID || entry->index >= system->fonts.length) {
    if (remove_entry && name.str) {
      vkr_hash_table_remove_VkrFontSystemEntry(&system->font_map,
                                               (const char *)name.str);
    }
    return;
  }

  VkrFont *font = &system->fonts.data[entry->index];
  if (font->generation != VKR_INVALID_ID) {
    if (font->type == VKR_FONT_TYPE_BITMAP ||
        font->type == VKR_FONT_TYPE_SYSTEM ||
        font->type == VKR_FONT_TYPE_MTSDF) {
      VkrResourceType res_type = VKR_RESOURCE_TYPE_UNKNOWN;
      if (font->type == VKR_FONT_TYPE_SYSTEM) {
        res_type = VKR_RESOURCE_TYPE_SYSTEM_FONT;
      } else if (font->type == VKR_FONT_TYPE_BITMAP) {
        res_type = VKR_RESOURCE_TYPE_BITMAP_FONT;
      } else {
        res_type = VKR_RESOURCE_TYPE_MTSDF_FONT;
      }
      uint32_t loader_id = entry->loader_id;
      if (loader_id == VKR_INVALID_ID) {
        loader_id = vkr_resource_system_get_loader_id(res_type, name);
      }
      VkrResourceHandleInfo handle_info = {
          .type = res_type,
          .loader_id = loader_id,
          .as.custom = entry->resource,
      };
      vkr_resource_system_unload(&handle_info, name);
    } else {
      log_warn("Font system: unsupported font type %u for unload",
               (unsigned)font->type);
    }
  }

  uint32_t freed_index = entry->index;

  MemZero(font, sizeof(*font));
  font->id = VKR_INVALID_ID;
  font->generation = VKR_INVALID_ID;

  entry->index = VKR_INVALID_ID;
  entry->ref_count = 0;
  entry->auto_release = false_v;
  entry->loader_id = VKR_INVALID_ID;
  entry->resource = NULL;

  if (freed_index < system->next_free_index) {
    system->next_free_index = freed_index;
  }

  if (remove_entry) {
    if (freed_index == VKR_INVALID_ID || freed_index >= system->fonts.length) {
      if (name.str) {
        vkr_hash_table_remove_VkrFontSystemEntry(&system->font_map,
                                                 (const char *)name.str);
      }
      return;
    }

    for (uint64_t i = 0; i < system->font_map.capacity; i++) {
      VkrHashEntry_VkrFontSystemEntry *map_entry = &system->font_map.entries[i];
      if (map_entry->occupied != VKR_OCCUPIED) {
        continue;
      }

      if (map_entry->value.index != freed_index) {
        continue;
      }

      if (map_entry->key) {
        vkr_hash_table_remove_VkrFontSystemEntry(&system->font_map,
                                                 map_entry->key);
      }
    }
  }
}

// =============================================================================
// Initialization / Shutdown
// =============================================================================

bool8_t vkr_font_system_init(VkrFontSystem *system,
                             VkrRendererFrontendHandle renderer,
                             const VkrFontSystemConfig *config,
                             VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  MemZero(system, sizeof(*system));

  uint32_t max_fonts =
      config->max_system_font_count + config->max_bitmap_font_count;
  if (max_fonts == 0) {
    log_error("Font system max font count must be greater than 0");
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  ArenaFlags arena_flags = bitset8_create();
  bitset8_set(&arena_flags, ARENA_FLAG_LARGE_PAGES);
  system->arena = arena_create(VKR_FONT_SYSTEM_DEFAULT_MEM,
                               VKR_FONT_SYSTEM_DEFAULT_MEM / 4, arena_flags);
  if (!system->arena) {
    log_fatal("Failed to create font system arena");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  system->allocator = (VkrAllocator){.ctx = system->arena};
  if (!vkr_allocator_arena(&system->allocator)) {
    log_fatal("Failed to create font system allocator");
    arena_destroy(system->arena);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  system->temp_arena = arena_create(MB(1), KB(256), arena_flags);
  if (!system->temp_arena) {
    log_fatal("Failed to create font system temp arena");
    arena_destroy(system->arena);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  system->temp_allocator = (VkrAllocator){.ctx = system->temp_arena};
  if (!vkr_allocator_arena(&system->temp_allocator)) {
    log_fatal("Failed to create font system temp allocator");
    arena_destroy(system->temp_arena);
    arena_destroy(system->arena);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  system->renderer = renderer;
  system->config = *config;
  system->job_system = NULL;

  system->fonts = array_create_VkrFont(&system->allocator, max_fonts);
  if (!system->fonts.data) {
    log_fatal("Failed to allocate fonts array");
    arena_destroy(system->arena);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  for (uint32_t i = 0; i < max_fonts; i++) {
    system->fonts.data[i].id = VKR_INVALID_ID;
    system->fonts.data[i].generation = VKR_INVALID_ID;
  }

  system->font_map = vkr_hash_table_create_VkrFontSystemEntry(
      &system->allocator, ((uint64_t)max_fonts) * 2ULL);

  system->next_free_index = 0;
  system->generation_counter = 1;

  String8 font_name = string8_lit("NotoSansCJK");
  String8 fontcfg_path = string8_lit("assets/fonts/NotoSansCJK.fontcfg");
  VkrRendererError font_load_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_font_system_load_from_file(system, font_name, fontcfg_path,
                                      &font_load_error)) {
    String8 err_str = vkr_renderer_get_error_string(font_load_error);
    log_error("Failed to load default system UI font: %s",
              string8_cstr(&err_str));
    *out_error = font_load_error;
    return false_v;
  }

  VkrRendererError font_acq_error = VKR_RENDERER_ERROR_NONE;
  system->default_system_font_handle =
      vkr_font_system_acquire(system, font_name, true_v, &font_acq_error);
  if (font_acq_error != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(font_acq_error);
    log_error("Failed to acquire default system UI font: %s",
              string8_cstr(&err_str));
    *out_error = font_acq_error;
    return false_v;
  }

  String8 font_bitmap_name = string8_lit("UbuntuMono-bitmap");
  String8 fontcfg_bitmap_path =
      string8_lit("assets/fonts/UbuntuMono-bitmap.fontcfg");
  if (!vkr_font_system_load_from_file(system, font_bitmap_name,
                                      fontcfg_bitmap_path, &font_load_error)) {
    String8 err_str = vkr_renderer_get_error_string(font_load_error);
    log_error("Failed to load default bitmap UI font: %s",
              string8_cstr(&err_str));
    *out_error = font_load_error;
    return false_v;
  }

  system->default_bitmap_font_handle = vkr_font_system_acquire(
      system, font_bitmap_name, true_v, &font_acq_error);
  if (font_acq_error != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(font_acq_error);
    log_error("Failed to acquire default bitmap UI font: %s",
              string8_cstr(&err_str));
    *out_error = font_acq_error;
    return false_v;
  }

  String8 font_mtsdf_name = string8_lit("UbuntuMono-mtsdf");
  String8 fontcfg_mtsdf_path =
      string8_lit("assets/fonts/UbuntuMono-2d.fontcfg");
  if (!vkr_font_system_load_from_file(system, font_mtsdf_name,
                                      fontcfg_mtsdf_path, &font_load_error)) {
    String8 err_str = vkr_renderer_get_error_string(font_load_error);
    log_error("Failed to load default mtsdf UI font: %s",
              string8_cstr(&err_str));
    *out_error = font_load_error;
    return false_v;
  }

  system->default_mtsdf_font_handle =
      vkr_font_system_acquire(system, font_mtsdf_name, true_v, &font_acq_error);
  if (font_acq_error != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(font_acq_error);
    log_error("Failed to acquire default mtsdf UI font: %s",
              string8_cstr(&err_str));
    *out_error = font_acq_error;
    return false_v;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

void vkr_font_system_shutdown(VkrFontSystem *system) {
  if (!system) {
    return;
  }

  if (system->default_system_font_handle.id != 0) {
    vkr_font_system_release_by_handle(system,
                                      system->default_system_font_handle);
    system->default_system_font_handle = VKR_FONT_HANDLE_INVALID;
  }

  if (system->default_bitmap_font_handle.id != 0) {
    vkr_font_system_release_by_handle(system,
                                      system->default_bitmap_font_handle);
    system->default_bitmap_font_handle = VKR_FONT_HANDLE_INVALID;
  }

  if (system->default_mtsdf_font_handle.id != 0) {
    vkr_font_system_release_by_handle(system,
                                      system->default_mtsdf_font_handle);
    system->default_mtsdf_font_handle = VKR_FONT_HANDLE_INVALID;
  }

  if (system->font_map.entries) {
    for (uint64_t i = 0; i < system->font_map.capacity; i++) {
      VkrHashEntry_VkrFontSystemEntry *entry = &system->font_map.entries[i];
      if (entry->occupied == VKR_OCCUPIED && entry->key) {
        String8 name = string8_create_from_cstr((const uint8_t *)entry->key,
                                                string_length(entry->key));
        vkr_font_system_unload_font(system, &entry->value, name, false_v);
      }
    }
  }

  array_destroy_VkrFont(&system->fonts);
  vkr_hash_table_destroy_VkrFontSystemEntry(&system->font_map);

  if (system->temp_arena) {
    arena_destroy(system->temp_arena);
  }

  if (system->arena) {
    arena_destroy(system->arena);
  }

  MemZero(system, sizeof(*system));
}

// =============================================================================
// Resource operations
// =============================================================================

VkrFontHandle vkr_font_system_acquire(VkrFontSystem *system, String8 name,
                                      bool8_t auto_release,
                                      VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  const char *font_key = (const char *)name.str;
  VkrFontSystemEntry *entry =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);
  if (!entry) {
    log_warn("Font '%s' not yet loaded, use load_from_file first", font_key);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return VKR_FONT_HANDLE_INVALID;
  }

  if (entry->ref_count == 0) {
    entry->auto_release = auto_release;
  }
  entry->ref_count++;

  VkrFont *font = &system->fonts.data[entry->index];
  *out_error = VKR_RENDERER_ERROR_NONE;
  return (VkrFontHandle){.id = font->id, .generation = font->generation};
}

void vkr_font_system_release(VkrFontSystem *system, String8 name) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  const char *font_key = (const char *)name.str;
  VkrFontSystemEntry *entry =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);

  if (!entry) {
    log_warn("Attempted to release unknown font '%s'", font_key);
    return;
  }

  if (entry->ref_count == 0) {
    log_warn("Over-release detected for font '%s'", font_key);
    return;
  }

  entry->ref_count--;

  if (entry->ref_count == 0 && entry->auto_release) {
    bool8_t should_unload = true_v;
    uint32_t font_index = entry->index;
    if (font_index != VKR_INVALID_ID && font_index < system->fonts.length) {
      for (uint64_t i = 0; i < system->font_map.capacity; i++) {
        VkrHashEntry_VkrFontSystemEntry *map_entry =
            &system->font_map.entries[i];
        if (map_entry->occupied != VKR_OCCUPIED) {
          continue;
        }
        if (map_entry->value.index != font_index) {
          continue;
        }
        if (map_entry->value.ref_count > 0 || !map_entry->value.auto_release) {
          should_unload = false_v;
          break;
        }
      }
    }

    if (should_unload) {
      vkr_font_system_unload_font(system, entry, name, true_v);
    }
  }
}

void vkr_font_system_release_by_handle(VkrFontSystem *system,
                                       VkrFontHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == 0 || handle.id == VKR_INVALID_ID) {
    log_warn("Attempted to release invalid font handle");
    return;
  }

  bool8_t found = false_v;
  for (uint64_t i = 0; i < system->font_map.capacity; i++) {
    VkrHashEntry_VkrFontSystemEntry *entry = &system->font_map.entries[i];
    if (entry->occupied != VKR_OCCUPIED) {
      continue;
    }

    uint32_t font_index = entry->value.index;
    if (font_index >= system->fonts.length) {
      continue;
    }

    VkrFont *font = &system->fonts.data[font_index];
    if (font->id == handle.id && font->generation == handle.generation) {
      found = true_v;
      if (entry->value.ref_count == 0) {
        continue;
      }
      String8 name = string8_create_from_cstr((const uint8_t *)entry->key,
                                              string_length(entry->key));
      vkr_font_system_release(system, name);
      return;
    }
  }

  if (found) {
    log_warn("Over-release detected for font handle");
  } else {
    log_warn("Font handle not found in system");
  }
}

bool8_t vkr_font_system_load_from_file(VkrFontSystem *system, String8 name,
                                       String8 fontcfg_path,
                                       VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(fontcfg_path.str != NULL, "Config path is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (name.length == 0 || fontcfg_path.length == 0) {
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  const char *font_key = (const char *)name.str;
  VkrFontSystemEntry *existing =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);
  if (existing) {
    log_warn("Font '%s' already loaded", font_key);
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  VkrAllocatorScope parse_scope =
      vkr_allocator_begin_scope(&system->temp_allocator);
  VkrFontConfig config = vkr_font_config_parse(fontcfg_path, &system->allocator,
                                               &system->temp_allocator);
  vkr_allocator_end_scope(&parse_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);

  if (!config.is_valid) {
    log_error("Failed to parse font config '%.*s'",
              (int32_t)fontcfg_path.length, fontcfg_path.str);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    return false_v;
  }

  if (config.type == VKR_FONT_TYPE_SYSTEM) {
    VkrAllocatorScope load_scope =
        vkr_allocator_begin_scope(&system->temp_allocator);
    if (!vkr_allocator_scope_is_valid(&load_scope)) {
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }

    uint32_t font_count = vkr_font_system_get_font_count_from_file(
        config.file, &system->temp_allocator);
    if (font_count == 0) {
      log_error("Failed to read font file '%.*s'", (int32_t)config.file.length,
                config.file.str);
      vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      *out_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
      return false_v;
    }

    uint32_t variants_to_load = font_count;
    if (config.face_count > 0 && config.face_count < font_count) {
      variants_to_load = config.face_count;
    }

    uint32_t loaded = 0;
    bool8_t name_registered = false_v;

    for (uint32_t i = 0; i < variants_to_load; i++) {
      String8 variant_name = {0};
      if (i < config.face_count && config.faces[i].str &&
          config.faces[i].length > 0) {
        variant_name = config.faces[i];
      } else {
        variant_name =
            string8_create_formatted(&system->temp_allocator, "%.*s-%u",
                                     (int32_t)name.length, name.str, i);
      }

      VkrRendererError variant_error = VKR_RENDERER_ERROR_NONE;
      if (vkr_font_system_load_single_variant(system, variant_name, &config, i,
                                              &variant_error)) {
        loaded++;

        if (name_registered) {
          continue;
        }

        if (!string8_equals(&name, &variant_name)) {
          const char *variant_key = (const char *)variant_name.str;
          VkrFontSystemEntry *variant_entry =
              vkr_hash_table_get_VkrFontSystemEntry(&system->font_map,
                                                    variant_key);
          if (variant_entry) {
            char *alias_key =
                (char *)vkr_allocator_alloc(&system->allocator, name.length + 1,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
            if (alias_key) {
              MemCopy(alias_key, name.str, name.length);
              alias_key[name.length] = '\0';

              VkrFontSystemEntry alias_entry = *variant_entry;
              alias_entry.ref_count = 0;
              if (!vkr_hash_table_insert_VkrFontSystemEntry(
                      &system->font_map, alias_key, alias_entry)) {
                log_warn("Failed to register font alias '%s'", alias_key);
              }
            } else {
              log_warn("Failed to allocate font alias '%.*s'",
                       (int32_t)name.length, name.str);
            }
          }
        }

        name_registered = true_v;
      } else {
        log_warn("Failed to load font variant %u from '%.*s': error %d", i,
                 (int32_t)config.file.length, config.file.str,
                 (int)variant_error);
      }
    }

    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);

    if (loaded == 0) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }

    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  uint32_t free_slot = vkr_font_system_find_free_slot(system);
  if (free_slot == VKR_INVALID_ID) {
    log_error("Font system is full (max=%llu)",
              (unsigned long long)system->fonts.length);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  VkrResourceType resource_type = VKR_RESOURCE_TYPE_UNKNOWN;
  switch (config.type) {
  case VKR_FONT_TYPE_BITMAP:
    resource_type = VKR_RESOURCE_TYPE_BITMAP_FONT;
    break;
  case VKR_FONT_TYPE_MTSDF:
    resource_type = VKR_RESOURCE_TYPE_MTSDF_FONT;
    break;
  default:
    log_error("Unknown font type");
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
    return false_v;
  }

  VkrAllocatorScope load_scope =
      vkr_allocator_begin_scope(&system->temp_allocator);
  if (!vkr_allocator_scope_is_valid(&load_scope)) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  String8 load_name = config.file;
  if (config.type == VKR_FONT_TYPE_MTSDF) {
    if (!config.atlas.str || config.atlas.length == 0) {
      log_error("Font config: missing atlas for mtsdf '%.*s'",
                (int32_t)fontcfg_path.length, fontcfg_path.str);
      *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      return false_v;
    }

    uint32_t size = config.size > 0 ? config.size : VKR_MTSDF_FONT_DEFAULT_SIZE;
    load_name = string8_create_formatted(
        &system->temp_allocator, "%.*s?atlas=%.*s&size=%u",
        (int32_t)config.file.length, config.file.str,
        (int32_t)config.atlas.length, config.atlas.str, size);
  }

  VkrResourceHandleInfo handle_info = {0};
  VkrRendererError load_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load(resource_type, load_name, &system->allocator,
                                &handle_info, &load_error)) {
    String8 err = vkr_renderer_get_error_string(load_error);
    log_error("Failed to load font '%.*s': %s", (int32_t)load_name.length,
              load_name.str, string8_cstr(&err));
    *out_error = (load_error != VKR_RENDERER_ERROR_NONE)
                     ? load_error
                     : VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }

  VkrFont *loaded_font = NULL;
  VkrRendererError result_error = VKR_RENDERER_ERROR_NONE;
  bool8_t result_success = false_v;

  if (config.type == VKR_FONT_TYPE_BITMAP) {
    VkrBitmapFontLoaderResult *result =
        (VkrBitmapFontLoaderResult *)handle_info.as.custom;
    if (result) {
      result_success = result->success;
      result_error = result->error;
      loaded_font = &result->font;
    }
  } else if (config.type == VKR_FONT_TYPE_MTSDF) {
    VkrMtsdfFontLoaderResult *result =
        (VkrMtsdfFontLoaderResult *)handle_info.as.custom;
    if (result) {
      result_success = result->success;
      result_error = result->error;
      loaded_font = &result->font;
    }
  }

  if (!result_success || !loaded_font) {
    vkr_resource_system_unload(&handle_info, load_name);
    *out_error = (result_error != VKR_RENDERER_ERROR_NONE)
                     ? result_error
                     : VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }

  VkrFont *font = &system->fonts.data[free_slot];
  *font = *loaded_font;

  font->id = free_slot + 1;
  font->generation = system->generation_counter++;

  char *stable_key = (char *)vkr_allocator_alloc(
      &system->allocator, name.length + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_key) {
    log_error("Failed to allocate key for font map");
    vkr_resource_system_unload(&handle_info, load_name);
    MemZero(font, sizeof(*font));
    font->id = VKR_INVALID_ID;
    font->generation = VKR_INVALID_ID;
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }
  MemCopy(stable_key, name.str, (size_t)name.length);
  stable_key[name.length] = '\0';

  VkrFontSystemEntry entry = {
      .index = free_slot,
      .ref_count = 0,
      .auto_release = true_v,
      .loader_id = handle_info.loader_id,
      .resource = handle_info.as.custom,
  };

  if (!vkr_hash_table_insert_VkrFontSystemEntry(&system->font_map, stable_key,
                                                entry)) {
    log_error("Failed to insert font '%s' into hash table", stable_key);
    vkr_resource_system_unload(&handle_info, load_name);
    MemZero(font, sizeof(*font));
    font->id = VKR_INVALID_ID;
    font->generation = VKR_INVALID_ID;
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    return false_v;
  }

  *out_error = VKR_RENDERER_ERROR_NONE;
  vkr_allocator_end_scope(&load_scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  return true_v;
}

uint32_t vkr_font_system_load_batch(VkrFontSystem *system, const String8 *names,
                                    const String8 *fontcfg_paths,
                                    uint32_t count, VkrFontHandle *out_handles,
                                    VkrRendererError *out_errors) {
  assert_log(system != NULL, "System is NULL");
  assert_log(names != NULL, "Names is NULL");
  assert_log(fontcfg_paths != NULL, "Config paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  if (count == 0) {
    return 0;
  }

  for (uint32_t i = 0; i < count; i++) {
    out_handles[i] = VKR_FONT_HANDLE_INVALID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!names[i].str || !fontcfg_paths[i].str) {
      out_errors[i] = VKR_RENDERER_ERROR_INVALID_PARAMETER;
      continue;
    }

    if (vkr_font_system_load_from_file(system, names[i], fontcfg_paths[i],
                                       &out_errors[i])) {
      VkrFontSystemEntry *entry = vkr_hash_table_get_VkrFontSystemEntry(
          &system->font_map, (const char *)names[i].str);
      if (entry) {
        VkrFont *font = &system->fonts.data[entry->index];
        out_handles[i] =
            (VkrFontHandle){.id = font->id, .generation = font->generation};
        loaded++;
      }
    }
  }

  return loaded;
}

// =============================================================================
// Validation
// =============================================================================

bool8_t vkr_font_system_validate_atlas(VkrFontSystem *system,
                                       VkrFontHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != VKR_INVALID_ID, "Handle is invalid");

  VkrFont *font = vkr_font_system_get_by_handle(system, handle);
  if (!font) {
    return false_v;
  }

  if (font->atlas.id == 0 || font->atlas.id == VKR_INVALID_ID ||
      font->atlas.generation == VKR_INVALID_ID) {
    return false_v;
  }

  if (font->atlas_pages.data) {
    for (uint32_t i = 0; i < font->atlas_pages.length; i++) {
      VkrTextureHandle page = font->atlas_pages.data[i];
      if (page.id == 0 || page.id == VKR_INVALID_ID ||
          page.generation == VKR_INVALID_ID) {
        return false_v;
      }
    }
  }

  return true_v;
}

bool8_t vkr_font_system_validate_glyphs(VkrFontSystem *system,
                                        VkrFontHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != VKR_INVALID_ID, "Handle is invalid");

  VkrFont *font = vkr_font_system_get_by_handle(system, handle);
  if (!font) {
    return false_v;
  }

  if (!font->glyphs.data || font->glyphs.length == 0) {
    return false_v;
  }

  bool8_t has_space = false_v;
  for (uint64_t i = 0; i < font->glyphs.length; i++) {
    if (font->glyphs.data[i].codepoint == 32) {
      has_space = true_v;
      break;
    }
  }

  return has_space;
}

bool8_t vkr_font_system_is_valid(VkrFontSystem *system, VkrFontHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != VKR_INVALID_ID, "Handle is invalid");

  VkrFont *font = vkr_font_system_get_by_handle(system, handle);
  if (!font) {
    return false_v;
  }

  if (font->size == 0 || font->line_height <= 0) {
    return false_v;
  }

  if (!vkr_font_system_validate_atlas(system, handle)) {
    return false_v;
  }

  if (!vkr_font_system_validate_glyphs(system, handle)) {
    return false_v;
  }

  return true_v;
}

// =============================================================================
// Getters
// =============================================================================

VkrFontHandle vkr_font_system_acquire_by_handle(VkrFontSystem *system,
                                                VkrFontHandle handle,
                                                VkrRendererError *out_error) {
  assert_log(system != NULL, "System is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrFont *font = vkr_font_system_get_by_handle(system, handle);
  if (!font) {
    *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    return VKR_FONT_HANDLE_INVALID;
  }

  for (uint64_t i = 0; i < system->font_map.capacity; i++) {
    VkrHashEntry_VkrFontSystemEntry *entry = &system->font_map.entries[i];
    if (entry->occupied == VKR_OCCUPIED &&
        entry->value.index == (handle.id - 1)) {
      entry->value.ref_count++;
      *out_error = VKR_RENDERER_ERROR_NONE;
      return handle;
    }
  }

  *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
  return VKR_FONT_HANDLE_INVALID;
}

VkrFont *vkr_font_system_get_by_handle(VkrFontSystem *system,
                                       VkrFontHandle handle) {
  assert_log(system != NULL, "System is NULL");

  if (handle.id == VKR_INVALID_ID) {
    return NULL;
  }

  uint32_t index = handle.id - 1;
  if (index >= system->fonts.length) {
    return NULL;
  }

  VkrFont *font = vkr_font_system_get_by_index(system, index);
  if (!font) {
    return NULL;
  }
  if (font->generation != handle.generation) {
    return NULL;
  }

  return font;
}

VkrFont *vkr_font_system_get_by_name(VkrFontSystem *system, String8 name) {
  assert_log(system != NULL, "System is NULL");

  if (!name.str) {
    return NULL;
  }

  const char *font_key = (const char *)name.str;
  VkrFontSystemEntry *entry =
      vkr_hash_table_get_VkrFontSystemEntry(&system->font_map, font_key);
  if (!entry || entry->index == VKR_INVALID_ID) {
    return NULL;
  }

  return &system->fonts.data[entry->index];
}

VkrFont *vkr_font_system_get_default_system_font(VkrFontSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return vkr_font_system_get_by_handle(system,
                                       system->default_system_font_handle);
}

VkrFont *vkr_font_system_get_default_bitmap_font(VkrFontSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return vkr_font_system_get_by_handle(system,
                                       system->default_bitmap_font_handle);
}

VkrFont *vkr_font_system_get_default_mtsdf_font(VkrFontSystem *system) {
  assert_log(system != NULL, "System is NULL");
  return vkr_font_system_get_by_handle(system,
                                       system->default_mtsdf_font_handle);
}
