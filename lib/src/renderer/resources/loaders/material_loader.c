#include "renderer/resources/loaders/material_loader.h"
#include "containers/str.h"
#include "renderer/systems/vkr_material_system.h"

#define VKR_MATERIAL_EXTENSION "mt"

/**
 * @brief Maximum path length for texture paths in parsed material data.
 */
#define VKR_MATERIAL_PATH_MAX 512

/**
 * @brief Intended sampling color space for material textures.
 */
typedef enum VkrMaterialTextureColorSpace {
  VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR = 0,
  VKR_MATERIAL_TEXTURE_COLORSPACE_SRGB = 1,
} VkrMaterialTextureColorSpace;

/**
 * @brief Context for batch material loading operations.
 */
typedef struct VkrMaterialBatchContext {
  struct VkrMaterialSystem *material_system;
  VkrJobSystem *job_system;
  VkrAllocator *allocator;
  VkrAllocator *temp_allocator;
} VkrMaterialBatchContext;

/**
 * @brief Parsed material data before textures are loaded.
 * Used for batch loading to separate parsing from GPU upload.
 */
typedef struct VkrParsedMaterialData {
  char name[VKR_MATERIAL_NAME_MAX];
  char shader_name[VKR_MATERIAL_NAME_MAX];
  uint32_t pipeline_id;
  VkrPhongProperties phong;
  float32_t alpha_cutoff;
  bool8_t alpha_cutoff_set;
  bool8_t cutout_enabled;

  // Texture paths as fixed buffers (thread-safe for parallel parsing)
  char diffuse_path[VKR_MATERIAL_PATH_MAX];
  char specular_path[VKR_MATERIAL_PATH_MAX];
  char normal_path[VKR_MATERIAL_PATH_MAX];
  VkrMaterialTextureColorSpace diffuse_colorspace;
  VkrMaterialTextureColorSpace specular_colorspace;
  VkrMaterialTextureColorSpace normal_colorspace;

  bool8_t parse_success;
  VkrRendererError parse_error;
} VkrParsedMaterialData;

/**
 * @brief Job payload for parallel material file parsing.
 */
typedef struct VkrMaterialParseJobPayload {
  char file_path[VKR_MATERIAL_PATH_MAX];
  VkrParsedMaterialData *result;
} VkrMaterialParseJobPayload;

typedef struct VkrMaterialAsyncDependency {
  bool8_t requested;
  VkrTextureSlot slot;
  char resolved_path[VKR_MATERIAL_PATH_MAX];
  VkrResourceHandleInfo request_info;
} VkrMaterialAsyncDependency;

typedef struct VkrMaterialLoaderAsyncPayload {
  VkrParsedMaterialData parsed;
  VkrMaterialAsyncDependency dependencies[VKR_TEXTURE_SLOT_COUNT];
} VkrMaterialLoaderAsyncPayload;

vkr_internal uint32_t vkr_material_loader_load_batch(
    VkrMaterialBatchContext *context, const String8 *material_paths,
    uint32_t count, VkrMaterialHandle *out_handles,
    VkrRendererError *out_errors);

vkr_internal bool8_t vkr_material_loader_parse_file(
    VkrAllocator *allocator, String8 path, VkrParsedMaterialData *out_data);

vkr_internal bool8_t vkr_material_loader_prepare_async(
    VkrResourceLoader *self, String8 name, VkrAllocator *temp_alloc,
    void **out_payload, VkrRendererError *out_error);

vkr_internal bool8_t vkr_material_loader_finalize_async(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error);

vkr_internal bool8_t vkr_material_loader_estimate_async_finalize_cost(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceAsyncFinalizeCost *out_cost);

vkr_internal void
vkr_material_loader_release_async_payload(VkrResourceLoader *self,
                                          void *payload);

vkr_internal void vkr_get_stable_material_name(char *material_name_buf,
                                               String8 name,
                                               String8 *out_name) {
  assert_log(material_name_buf != NULL, "Material name buffer is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(out_name != NULL, "Out name is NULL");

  const char *cpath = (const char *)name.str;
  const char *last_slash = string_get_last_char_occurrence(cpath, '/');
  const char *fname = last_slash ? last_slash + 1 : cpath;
  const char *dot = string_get_last_char_occurrence(fname, '.');
  if (dot && (size_t)(dot - fname) < VKR_MATERIAL_NAME_MAX) {
    MemCopy(material_name_buf, fname, (size_t)(dot - fname));
    material_name_buf[dot - fname] = '\0';
  } else {
    size_t flen = string_length(fname);
    size_t copy_len =
        flen < VKR_MATERIAL_NAME_MAX - 1 ? flen : VKR_MATERIAL_NAME_MAX - 1;
    MemCopy(material_name_buf, fname, copy_len);
    material_name_buf[copy_len] = '\0';
  }

  *out_name = string8_create_from_cstr((const uint8_t *)material_name_buf,
                                       string_length(material_name_buf));
}

vkr_internal uint32_t vkr_get_pipeline_id_from_string(char *value) {
  assert_log(value != NULL, "Value is NULL");
  assert_log(strlen(value) > 0, "Value is empty");

  if (string_equalsi(value, "world")) {
    return VKR_PIPELINE_DOMAIN_WORLD;
  } else if (string_equalsi(value, "ui")) {
    return VKR_PIPELINE_DOMAIN_UI;
  } else if (string_equalsi(value, "compute")) {
    return VKR_PIPELINE_DOMAIN_COMPUTE;
  } else if (string_equalsi(value, "shadow")) {
    return VKR_PIPELINE_DOMAIN_SHADOW;
  } else if (string_equalsi(value, "post")) {
    return VKR_PIPELINE_DOMAIN_POST;
  }

  return VKR_INVALID_ID;
}

vkr_internal void vkr_material_cleanup_shader_name(VkrMaterialSystem *system,
                                                   const char *shader_name) {
  if (!shader_name ||
      !vkr_dmemory_owns_ptr(&system->string_memory, (void *)shader_name)) {
    return;
  }
  uint64_t len = string_length(shader_name) + 1;
  vkr_allocator_free(&system->string_allocator, (void *)shader_name, len,
                     VKR_ALLOCATOR_MEMORY_TAG_STRING);
}

vkr_internal uint32_t vkr_material_find_slot(VkrMaterialSystem *system) {
  if (system->free_count > 0) {
    uint32_t slot = system->free_ids.data[system->free_count - 1];
    system->free_count--;
    return slot;
  }

  uint32_t slot = system->next_free_index;
  while (slot < system->materials.length &&
         system->materials.data[slot].id != 0) {
    slot++;
  }
  if (slot >= system->materials.length) {
    return VKR_INVALID_ID;
  }
  system->next_free_index = slot + 1;
  return slot;
}

vkr_internal void vkr_material_init_defaults(VkrMaterial *material,
                                             VkrMaterialSystem *system) {
  MemZero(material, sizeof(*material));
  material->phong.diffuse_color = vec4_new(1, 1, 1, 1);
  material->phong.specular_color = vec4_new(1, 1, 1, 1);
  material->phong.shininess = 32.0f;
  material->phong.emission_color = vec3_new(0, 0, 0);
  material->alpha_cutoff = 0.0f;

  for (uint32_t tex_slot = 0; tex_slot < VKR_TEXTURE_SLOT_COUNT; tex_slot++) {
    material->textures[tex_slot].slot = (VkrTextureSlot)tex_slot;
    material->textures[tex_slot].handle = VKR_TEXTURE_HANDLE_INVALID;
    material->textures[tex_slot].enabled = false;
  }

  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      vkr_texture_system_get_default_diffuse_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true;
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].slot = VKR_TEXTURE_SLOT_DIFFUSE;

  material->textures[VKR_TEXTURE_SLOT_NORMAL].handle =
      vkr_texture_system_get_default_normal_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled = true;
  material->textures[VKR_TEXTURE_SLOT_NORMAL].slot = VKR_TEXTURE_SLOT_NORMAL;

  material->textures[VKR_TEXTURE_SLOT_SPECULAR].handle =
      vkr_texture_system_get_default_specular_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_SPECULAR].enabled = true;
  material->textures[VKR_TEXTURE_SLOT_SPECULAR].slot =
      VKR_TEXTURE_SLOT_SPECULAR;
}

vkr_internal bool8_t vkr_material_parse_line_key_value(String8 line,
                                                       String8 *out_key,
                                                       String8 *out_value) {
  uint64_t eq = 0;
  bool found = false;
  for (uint64_t ch = 0; ch < line.length; ch++) {
    if (line.str[ch] == '=') {
      eq = ch;
      found = true;
      break;
    }
  }

  if (!found || eq == 0 || eq + 1 >= line.length) {
    return false_v;
  }

  *out_key = string8_substring(&line, 0, eq);
  *out_value = string8_substring(&line, eq + 1, line.length);
  string8_trim(out_key);
  string8_trim(out_value);
  return true_v;
}

vkr_internal bool8_t vkr_material_parse_colorspace_value(
    String8 value, VkrMaterialTextureColorSpace *out_colorspace) {
  assert_log(out_colorspace != NULL, "Out colorspace is NULL");

  String8 val_srgb = string8_lit("srgb");
  String8 val_linear = string8_lit("linear");
  if (string8_equalsi(&value, &val_srgb)) {
    *out_colorspace = VKR_MATERIAL_TEXTURE_COLORSPACE_SRGB;
    return true_v;
  }
  if (string8_equalsi(&value, &val_linear)) {
    *out_colorspace = VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR;
    return true_v;
  }

  return false_v;
}

vkr_internal void vkr_material_parse_colorspace_field(
    String8 key, String8 value, String8 material_name,
    VkrMaterialTextureColorSpace *out_colorspace, const char *field_name) {
  VkrMaterialTextureColorSpace parsed = VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR;
  if (vkr_material_parse_colorspace_value(value, &parsed)) {
    *out_colorspace = parsed;
  } else {
    log_warn("Material '%s': invalid %s_colorspace '%s'",
             string8_cstr(&material_name), field_name, string8_cstr(&value));
    *out_colorspace = VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR;
  }
}

vkr_internal VkrRendererError vkr_material_loader_load_from_mt(
    VkrResourceLoader *self, String8 path, VkrAllocator *temp_alloc,
    VkrMaterial *out_material);

vkr_internal bool8_t vkr_material_loader_can_load(VkrResourceLoader *self,
                                                  String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  const uint8_t *s = name.str;
  for (uint64_t ch = name.length; ch > 0; ch--) {
    if (s[ch - 1] == '.') {
      String8 ext = string8_substring(&name, ch, name.length);
      String8 mt =
          string8_create_from_cstr((const uint8_t *)VKR_MATERIAL_EXTENSION,
                                   string_length(VKR_MATERIAL_EXTENSION));
      return string8_equalsi(&ext, &mt);
    }
  }

  return false_v;
}

vkr_internal const char *
vkr_material_colorspace_to_string(VkrMaterialTextureColorSpace colorspace) {
  return colorspace == VKR_MATERIAL_TEXTURE_COLORSPACE_SRGB ? "srgb" : "linear";
}

/**
 * @brief Remove accidental `<type>|` request-key prefixes from texture paths.
 *
 * Material files should never store internal resource request keys, but stale
 * generated assets or malformed cache payloads can leak them. Stripping here
 * keeps dependency loads stable and prevents malformed filesystem paths.
 */
vkr_internal String8 vkr_material_strip_resource_key_prefix(String8 path) {
  if (!path.str || path.length < 3) {
    return path;
  }

  String8 stripped = path;
  for (uint32_t pass = 0; pass < 4; ++pass) {
    uint64_t pipe_index = UINT64_MAX;

    for (uint64_t segment_start = 0; segment_start < stripped.length;) {
      if (segment_start > 0) {
        uint8_t prev = stripped.str[segment_start - 1];
        if (prev != '/' && prev != '\\') {
          segment_start++;
          continue;
        }
      }

      uint64_t index = segment_start;
      while (index < stripped.length && stripped.str[index] >= '0' &&
             stripped.str[index] <= '9') {
        index++;
      }

      if (index > segment_start && index < stripped.length &&
          stripped.str[index] == '|') {
        pipe_index = index;
        break;
      }

      while (segment_start < stripped.length &&
             stripped.str[segment_start] != '/' &&
             stripped.str[segment_start] != '\\') {
        segment_start++;
      }
      if (segment_start < stripped.length) {
        segment_start++;
      }
    }

    if (pipe_index == UINT64_MAX || pipe_index + 1 >= stripped.length) {
      break;
    }

    stripped = string8_substring(&stripped, pipe_index + 1, stripped.length);
  }

  return stripped;
}

/**
 * @brief Strip the query portion from a texture path.
 * @param path The full path, potentially containing a query.
 * @param out_query Optional output for the query substring (without '?').
 * @return Base path without query parameters.
 */
vkr_internal String8 vkr_material_strip_query(String8 path,
                                              String8 *out_query) {
  for (uint64_t i = 0; i < path.length; ++i) {
    if (path.str[i] == '?') {
      if (out_query) {
        *out_query = string8_substring(&path, i + 1, path.length);
      }
      return string8_substring(&path, 0, i);
    }
  }

  if (out_query) {
    *out_query = (String8){0};
  }

  return path;
}

/**
 * @brief Find a `cs` parameter inside a query string.
 * @note Returns true if a `cs` key exists, even if the value is unknown.
 */
vkr_internal bool8_t vkr_material_query_get_colorspace(
    String8 query, VkrMaterialTextureColorSpace *out_colorspace,
    bool8_t *out_known) {
  assert_log(out_colorspace != NULL, "Out colorspace is NULL");

  bool8_t has_cs = false_v;
  bool8_t known = false_v;
  VkrMaterialTextureColorSpace parsed = VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR;

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
      String8 key_cs = string8_lit("cs");
      if (string8_equalsi(&key, &key_cs)) {
        has_cs = true_v;
        if (vkr_material_parse_colorspace_value(value, &parsed)) {
          known = true_v;
        } else {
          parsed = VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR;
          known = false_v;
        }
      }
    }

    start = end + 1;
  }

  *out_colorspace = parsed;
  if (out_known) {
    *out_known = known;
  }
  return has_cs;
}

/**
 * @brief Append a colorspace query to a texture path when requested.
 * @note If the path already has a `cs` parameter, it is treated as an override.
 */
vkr_internal String8
vkr_material_apply_colorspace(VkrAllocator *allocator, String8 path,
                              VkrMaterialTextureColorSpace colorspace,
                              String8 material_name, const char *slot_name) {
  path = vkr_material_strip_resource_key_prefix(path);
  if (!path.str || path.length == 0) {
    return path;
  }

  String8 query = {0};
  String8 base_path = vkr_material_strip_query(path, &query);
  bool8_t has_query = base_path.length != path.length;
  VkrMaterialTextureColorSpace parsed = VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR;
  bool8_t known = false_v;
  bool8_t has_cs = vkr_material_query_get_colorspace(query, &parsed, &known);

  if (has_cs) {
    if (known && parsed != colorspace) {
      log_warn(
          "Material '%.*s': %s_colorspace=%s conflicts with path override %s",
          (int32_t)material_name.length, material_name.str, slot_name,
          vkr_material_colorspace_to_string(colorspace),
          vkr_material_colorspace_to_string(parsed));
    }
    return path;
  }

  if (colorspace != VKR_MATERIAL_TEXTURE_COLORSPACE_SRGB) {
    return path;
  }

  if (!allocator) {
    return path;
  }

  return string8_create_formatted(allocator,
                                  has_query ? "%.*s&cs=srgb" : "%.*s?cs=srgb",
                                  (int32_t)path.length, path.str);
}

vkr_internal bool8_t vkr_material_copy_string8_to_path_buffer(String8 value,
                                                              char *out_path) {
  assert_log(out_path != NULL, "Output path buffer is NULL");

  out_path[0] = '\0';
  if (!value.str || value.length == 0) {
    return true_v;
  }
  if (value.length >= VKR_MATERIAL_PATH_MAX) {
    return false_v;
  }

  MemCopy(out_path, value.str, (size_t)value.length);
  out_path[value.length] = '\0';
  return true_v;
}

vkr_internal String8
vkr_material_make_string8_from_path_buffer(const char *path_buffer) {
  if (!path_buffer) {
    return (String8){0};
  }

  uint64_t length = string_length(path_buffer);
  if (length == 0) {
    return (String8){0};
  }

  return string8_create_from_cstr((const uint8_t *)path_buffer, length);
}

vkr_internal bool8_t vkr_material_copy_trimmed_value_to_cstr(
    String8 value, char *out_buffer, uint64_t out_buffer_size,
    uint32_t *out_length) {
  assert_log(out_buffer != NULL, "Output buffer is NULL");
  assert_log(out_buffer_size > 0, "Output buffer size must be > 0");

  String8 trimmed = value;
  string8_trim(&trimmed);

  if (!trimmed.str || trimmed.length == 0 ||
      trimmed.length >= out_buffer_size) {
    out_buffer[0] = '\0';
    if (out_length) {
      *out_length = 0;
    }
    return false_v;
  }

  MemCopy(out_buffer, trimmed.str, (size_t)trimmed.length);
  out_buffer[trimmed.length] = '\0';
  if (out_length) {
    *out_length = (uint32_t)trimmed.length;
  }
  return true_v;
}

vkr_internal void
vkr_material_loader_init_from_parsed(VkrMaterial *material,
                                     const VkrParsedMaterialData *parsed,
                                     VkrMaterialSystem *material_system) {
  assert_log(material != NULL, "Material is NULL");
  assert_log(parsed != NULL, "Parsed data is NULL");
  assert_log(material_system != NULL, "Material system is NULL");

  vkr_material_init_defaults(material, material_system);
  material->pipeline_id = parsed->pipeline_id;
  material->phong = parsed->phong;
  if (parsed->alpha_cutoff_set) {
    material->alpha_cutoff = parsed->alpha_cutoff;
  } else if (parsed->cutout_enabled) {
    material->alpha_cutoff = VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT;
  }
}

vkr_internal bool8_t vkr_material_loader_load(VkrResourceLoader *self,
                                              String8 name,
                                              VkrAllocator *temp_alloc,
                                              VkrResourceHandleInfo *out_handle,
                                              VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp allocator is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrMaterialSystem *system = (VkrMaterialSystem *)self->resource_system;

  VkrMaterial loaded_material = {0};
  *out_error = vkr_material_loader_load_from_mt(self, name, temp_alloc,
                                                &loaded_material);
  if (*out_error != VKR_RENDERER_ERROR_NONE) {
    return false_v;
  }

  char material_name_buf[VKR_MATERIAL_NAME_MAX] = {0};
  String8 material_name = {0};
  if (loaded_material.name) {
    uint64_t parsed_name_length = string_length(loaded_material.name);
    if (parsed_name_length > 0) {
      material_name = string8_create_from_cstr(
          (const uint8_t *)loaded_material.name, parsed_name_length);
    }
  }

  if (material_name.length == 0 || material_name.str == NULL) {
    vkr_get_stable_material_name(material_name_buf, name, &material_name);
  }

  const char *material_key = (const char *)material_name.str;
  VkrMaterialEntry *existing_entry = vkr_hash_table_get_VkrMaterialEntry(
      &system->material_by_name, material_key);
  if (existing_entry) {
    VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
    VkrMaterialHandle handle = vkr_material_system_acquire(
        system, material_name, true_v, &acquire_err);
    vkr_material_cleanup_shader_name(system, loaded_material.shader_name);
    if (handle.id == 0) {
      *out_error = acquire_err;
      return false_v;
    }
    out_handle->type = VKR_RESOURCE_TYPE_MATERIAL;
    out_handle->as.material = handle;
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  uint32_t slot = vkr_material_find_slot(system);
  if (slot == VKR_INVALID_ID) {
    log_error("Material system is full");
    vkr_material_cleanup_shader_name(system, loaded_material.shader_name);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  char *stable_name =
      vkr_allocator_alloc(&system->string_allocator, material_name.length + 1,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_name) {
    log_error("Failed to allocate name for material");
    vkr_material_cleanup_shader_name(system, loaded_material.shader_name);
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemCopy(stable_name, material_name.str, (size_t)material_name.length);
  stable_name[material_name.length] = '\0';

  VkrMaterial *material = &system->materials.data[slot];
  *material = loaded_material;
  material->name = (const char *)stable_name;

  material->id = slot + 1;
  material->generation = system->generation_counter++;

  VkrMaterialEntry new_entry = {.id = slot,
                                .ref_count = 0,
                                .auto_release = true_v,
                                .name = (const char *)stable_name};
  vkr_hash_table_insert_VkrMaterialEntry(&system->material_by_name,
                                         (const char *)stable_name, new_entry);

  VkrMaterialHandle handle = {.id = material->id,
                              .generation = material->generation};

  out_handle->type = VKR_RESOURCE_TYPE_MATERIAL;
  out_handle->loader_id = self->id;
  out_handle->as.material = handle;
  *out_error = VKR_RENDERER_ERROR_NONE;

  return true_v;
}

vkr_internal void vkr_material_loader_prepare_dependency(
    VkrMaterialLoaderAsyncPayload *payload, VkrTextureSlot slot,
    String8 raw_path, VkrMaterialTextureColorSpace colorspace,
    String8 material_name, const char *slot_name, VkrResourceLoader *loader,
    VkrAllocator *temp_alloc) {
  assert_log(payload != NULL, "Payload is NULL");
  assert_log(loader != NULL, "Loader is NULL");
  assert_log(temp_alloc != NULL, "Temp allocator is NULL");

  if (!raw_path.str || raw_path.length == 0) {
    return;
  }

  String8 resolved_path = vkr_material_apply_colorspace(
      temp_alloc, raw_path, colorspace, material_name, slot_name);

  VkrMaterialAsyncDependency *dependency = &payload->dependencies[slot];
  if (!vkr_material_copy_string8_to_path_buffer(resolved_path,
                                                dependency->resolved_path)) {
    log_warn(
        "Material '%.*s': %s path is too long and will use default texture",
        (int32_t)material_name.length, material_name.str, slot_name);
    return;
  }

  String8 dependency_path =
      vkr_material_make_string8_from_path_buffer(dependency->resolved_path);
  if (!dependency_path.str || dependency_path.length == 0) {
    return;
  }

  VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
  MemZero(&dependency->request_info, sizeof(dependency->request_info));
  dependency->request_info.type = VKR_RESOURCE_TYPE_TEXTURE;
  dependency->request_info.loader_id = VKR_INVALID_ID;
  dependency->request_info.load_state = VKR_RESOURCE_LOAD_STATE_INVALID;
  dependency->request_info.last_error = VKR_RENDERER_ERROR_NONE;
  dependency->request_info.request_id = 0;
  (void)vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE, dependency_path,
                                 temp_alloc, &dependency->request_info,
                                 &dependency_error);
  dependency->requested = true_v;
  dependency->slot = slot;
}

vkr_internal bool8_t vkr_material_loader_prepare_async(
    VkrResourceLoader *self, String8 name, VkrAllocator *temp_alloc,
    void **out_payload, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp allocator is NULL");
  assert_log(out_payload != NULL, "Out payload is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  *out_payload = NULL;
  *out_error = VKR_RENDERER_ERROR_NONE;

  VkrMaterialSystem *system = (VkrMaterialSystem *)self->resource_system;
  VkrMaterialLoaderAsyncPayload *payload =
      (VkrMaterialLoaderAsyncPayload *)vkr_allocator_alloc_ts(
          &system->async_allocator, sizeof(*payload),
          VKR_ALLOCATOR_MEMORY_TAG_STRUCT, system->async_mutex);
  if (!payload) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemZero(payload, sizeof(*payload));

  if (!vkr_material_loader_parse_file(temp_alloc, name, &payload->parsed) ||
      !payload->parsed.parse_success) {
    *out_error = payload->parsed.parse_error;
    vkr_allocator_free_ts(&system->async_allocator, payload, sizeof(*payload),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT, system->async_mutex);
    return false_v;
  }

  String8 material_name =
      string8_create_from_cstr((const uint8_t *)payload->parsed.name,
                               string_length(payload->parsed.name));

  vkr_material_loader_prepare_dependency(
      payload, VKR_TEXTURE_SLOT_DIFFUSE,
      vkr_material_make_string8_from_path_buffer(payload->parsed.diffuse_path),
      payload->parsed.diffuse_colorspace, material_name, "diffuse", self,
      temp_alloc);
  vkr_material_loader_prepare_dependency(
      payload, VKR_TEXTURE_SLOT_SPECULAR,
      vkr_material_make_string8_from_path_buffer(payload->parsed.specular_path),
      payload->parsed.specular_colorspace, material_name, "specular", self,
      temp_alloc);
  vkr_material_loader_prepare_dependency(
      payload, VKR_TEXTURE_SLOT_NORMAL,
      vkr_material_make_string8_from_path_buffer(payload->parsed.normal_path),
      payload->parsed.normal_colorspace, material_name, "normal", self,
      temp_alloc);

  *out_payload = payload;
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_material_loader_finalize_async(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceHandleInfo *out_handle, VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(payload != NULL, "Payload is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrMaterialSystem *system = (VkrMaterialSystem *)self->resource_system;
  VkrMaterialLoaderAsyncPayload *async_payload =
      (VkrMaterialLoaderAsyncPayload *)payload;
  const VkrParsedMaterialData *parsed = &async_payload->parsed;

  String8 material_name = string8_create_from_cstr(
      (const uint8_t *)parsed->name, string_length(parsed->name));
  if (!material_name.str || material_name.length == 0) {
    char material_name_buf[VKR_MATERIAL_NAME_MAX] = {0};
    vkr_get_stable_material_name(material_name_buf, name, &material_name);
  }

  VkrMaterialEntry *existing_entry = vkr_hash_table_get_VkrMaterialEntry(
      &system->material_by_name, (const char *)material_name.str);
  if (existing_entry) {
    VkrRendererError acquire_error = VKR_RENDERER_ERROR_NONE;
    VkrMaterialHandle acquired = vkr_material_system_acquire(
        system, material_name, true_v, &acquire_error);
    if (acquired.id == 0) {
      *out_error = acquire_error;
      return false_v;
    }
    out_handle->type = VKR_RESOURCE_TYPE_MATERIAL;
    out_handle->loader_id = self->id;
    out_handle->as.material = acquired;
    *out_error = VKR_RENDERER_ERROR_NONE;
    return true_v;
  }

  for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; ++i) {
    VkrMaterialAsyncDependency *dependency = &async_payload->dependencies[i];
    if (!dependency->requested) {
      continue;
    }

    VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state = vkr_resource_system_get_state(
        &dependency->request_info, &dependency_error);
    if (state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
        state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
        state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }
  }

  uint32_t slot = vkr_material_find_slot(system);
  if (slot == VKR_INVALID_ID) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  char *stable_name =
      vkr_allocator_alloc(&system->string_allocator, material_name.length + 1,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_name) {
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemCopy(stable_name, material_name.str, (size_t)material_name.length);
  stable_name[material_name.length] = '\0';

  char *stable_shader = NULL;
  uint64_t shader_name_len = string_length(parsed->shader_name);
  if (shader_name_len > 0) {
    stable_shader =
        vkr_allocator_alloc(&system->string_allocator, shader_name_len + 1,
                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (stable_shader) {
      MemCopy(stable_shader, parsed->shader_name, (size_t)shader_name_len);
      stable_shader[shader_name_len] = '\0';
    }
  }

  VkrTextureHandle acquired_textures[VKR_TEXTURE_SLOT_COUNT] = {
      VKR_TEXTURE_HANDLE_INVALID, VKR_TEXTURE_HANDLE_INVALID,
      VKR_TEXTURE_HANDLE_INVALID};

  VkrMaterial material = {0};
  vkr_material_loader_init_from_parsed(&material, parsed, system);

  for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; ++i) {
    VkrMaterialAsyncDependency *dependency = &async_payload->dependencies[i];
    if (!dependency->requested || dependency->resolved_path[0] == '\0') {
      continue;
    }

    VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
    VkrResourceLoadState state = vkr_resource_system_get_state(
        &dependency->request_info, &dependency_error);
    if (state != VKR_RESOURCE_LOAD_STATE_READY) {
      continue;
    }

    VkrResourceHandleInfo resolved_texture = {0};
    if (!vkr_resource_system_try_get_resolved(&dependency->request_info,
                                              &resolved_texture) ||
        resolved_texture.type != VKR_RESOURCE_TYPE_TEXTURE ||
        resolved_texture.as.texture.id == 0) {
      for (uint32_t j = 0; j < VKR_TEXTURE_SLOT_COUNT; ++j) {
        if (acquired_textures[j].id != 0) {
          vkr_texture_system_release_by_handle(system->texture_system,
                                               acquired_textures[j]);
          acquired_textures[j] = VKR_TEXTURE_HANDLE_INVALID;
        }
      }
      if (stable_shader &&
          vkr_dmemory_owns_ptr(&system->string_memory, (void *)stable_shader)) {
        vkr_allocator_free(&system->string_allocator, stable_shader,
                           shader_name_len + 1,
                           VKR_ALLOCATOR_MEMORY_TAG_STRING);
      }
      if (stable_name &&
          vkr_dmemory_owns_ptr(&system->string_memory, (void *)stable_name)) {
        vkr_allocator_free(&system->string_allocator, stable_name,
                           material_name.length + 1,
                           VKR_ALLOCATOR_MEMORY_TAG_STRING);
      }
      *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      return false_v;
    }

    VkrTextureHandle acquired = resolved_texture.as.texture;
    vkr_texture_system_add_ref_by_handle(system->texture_system, acquired);

    acquired_textures[dependency->slot] = acquired;
    material.textures[dependency->slot].handle = acquired;
    material.textures[dependency->slot].enabled = true;
  }

  VkrMaterial *dst = &system->materials.data[slot];
  *dst = material;
  dst->id = slot + 1;
  dst->generation = system->generation_counter++;
  dst->name = stable_name;
  dst->shader_name = stable_shader;

  VkrMaterialEntry new_entry = {
      .id = slot,
      .ref_count = 0,
      .auto_release = true_v,
      .name = stable_name,
  };
  if (!vkr_hash_table_insert_VkrMaterialEntry(&system->material_by_name,
                                              stable_name, new_entry)) {
    for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; ++i) {
      if (acquired_textures[i].id != 0) {
        vkr_texture_system_release_by_handle(system->texture_system,
                                             acquired_textures[i]);
      }
    }
    if (stable_shader &&
        vkr_dmemory_owns_ptr(&system->string_memory, (void *)stable_shader)) {
      vkr_allocator_free(&system->string_allocator, stable_shader,
                         shader_name_len + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
    vkr_allocator_free(&system->string_allocator, stable_name,
                       material_name.length + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    MemZero(dst, sizeof(*dst));
    if (system->free_count < system->free_ids.length) {
      system->free_ids.data[system->free_count++] = slot;
    }
    if (slot < system->next_free_index) {
      system->next_free_index = slot;
    }
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }

  out_handle->type = VKR_RESOURCE_TYPE_MATERIAL;
  out_handle->loader_id = self->id;
  out_handle->as.material = (VkrMaterialHandle){
      .id = dst->id,
      .generation = dst->generation,
  };
  *out_error = VKR_RENDERER_ERROR_NONE;
  return true_v;
}

vkr_internal bool8_t vkr_material_loader_estimate_async_finalize_cost(
    VkrResourceLoader *self, String8 name, void *payload,
    VkrResourceAsyncFinalizeCost *out_cost) {
  (void)self;
  (void)name;
  (void)payload;
  assert_log(out_cost != NULL, "Out cost is NULL");

  MemZero(out_cost, sizeof(*out_cost));
  return true_v;
}

vkr_internal void
vkr_material_loader_release_async_payload(VkrResourceLoader *self,
                                          void *payload) {
  assert_log(self != NULL, "Self is NULL");
  if (!payload) {
    return;
  }

  VkrMaterialLoaderAsyncPayload *async_payload =
      (VkrMaterialLoaderAsyncPayload *)payload;
  VkrMaterialSystem *system = (VkrMaterialSystem *)self->resource_system;
  for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; ++i) {
    VkrMaterialAsyncDependency *dependency = &async_payload->dependencies[i];
    if (!dependency->requested || dependency->resolved_path[0] == '\0') {
      continue;
    }

    String8 dependency_path =
        vkr_material_make_string8_from_path_buffer(dependency->resolved_path);
    if (dependency->request_info.request_id != 0 && dependency_path.str &&
        dependency_path.length > 0) {
      vkr_resource_system_unload(&dependency->request_info, dependency_path);
    }
  }

  vkr_allocator_free_ts(&system->async_allocator, async_payload,
                        sizeof(*async_payload), VKR_ALLOCATOR_MEMORY_TAG_STRUCT,
                        system->async_mutex);
}

vkr_internal void
vkr_material_loader_unload(VkrResourceLoader *self,
                           const VkrResourceHandleInfo *handle, String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  VkrMaterialSystem *system = (VkrMaterialSystem *)self->resource_system;

  char material_name_buf[VKR_MATERIAL_NAME_MAX] = {0};
  String8 material_name = {0};
  vkr_get_stable_material_name(material_name_buf, name, &material_name);

  const char *material_key = (const char *)material_name.str;
  VkrMaterialEntry *entry = vkr_hash_table_get_VkrMaterialEntry(
      &system->material_by_name, material_key);

  if (!entry) {
    log_debug("Material '%s' already released before loader unload",
              material_key);
    return;
  }

  /*
   * Resource-request unload must not tear down materials still referenced by
   * mesh assets/instances or scene components. Those owners release through the
   * material-system refcount API, which eventually calls this unloader again
   * when ref_count reaches zero.
   */
  if (entry->ref_count > 0) {
    return;
  }

  uint32_t material_index = entry->id;

  // Don't remove default material
  if (material_index == 0) {
    log_warn("Cannot remove default material");
    return;
  }

  // Reset material slot
  VkrMaterial *material = &system->materials.data[material_index];
  const char *stable_name = entry->name;
  const char *stable_shader = material->shader_name;

  for (uint32_t tex_slot = 0; tex_slot < VKR_TEXTURE_SLOT_COUNT; tex_slot++) {
    VkrTextureHandle handle = material->textures[tex_slot].handle;
    if (handle.id != 0) {
      vkr_texture_system_release_by_handle(system->texture_system, handle);
    }
  }

  material->id = 0;
  material->name = NULL;
  material->shader_name = NULL;
  material->pipeline_id = VKR_INVALID_ID;
  material->phong.diffuse_color = vec4_new(1, 1, 1, 1);
  material->phong.specular_color = vec4_new(1, 1, 1, 1);
  material->phong.shininess = 0.0f;
  material->phong.emission_color = vec3_new(0, 0, 0);
  material->alpha_cutoff = 0.0f;
  for (uint32_t tex_slot = 0; tex_slot < VKR_TEXTURE_SLOT_COUNT; tex_slot++) {
    material->textures[tex_slot].handle = VKR_TEXTURE_HANDLE_INVALID;
    material->textures[tex_slot].enabled = false;
    material->textures[tex_slot].slot = (VkrTextureSlot)tex_slot;
  }

  assert_log(system->free_count < system->free_ids.length,
             "free_ids overflow in material system");
  system->free_ids.data[system->free_count++] = material_index;

  vkr_hash_table_remove_VkrMaterialEntry(&system->material_by_name,
                                         material_key);

  if (stable_name &&
      vkr_dmemory_owns_ptr(&system->string_memory, (void *)stable_name)) {
    uint64_t len = string_length(stable_name) + 1;
    vkr_allocator_free(&system->string_allocator, (void *)stable_name, len,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  if (stable_shader &&
      vkr_dmemory_owns_ptr(&system->string_memory, (void *)stable_shader)) {
    uint64_t len = string_length(stable_shader) + 1;
    vkr_allocator_free(&system->string_allocator, (void *)stable_shader, len,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }

  if (material_index < system->next_free_index) {
    system->next_free_index = material_index;
  }
}

// Structure to hold pending texture paths for batch loading
typedef struct VkrMaterialTexturePaths {
  String8 diffuse;
  String8 specular;
  String8 normal;
  VkrMaterialTextureColorSpace diffuse_colorspace;
  VkrMaterialTextureColorSpace specular_colorspace;
  VkrMaterialTextureColorSpace normal_colorspace;
} VkrMaterialTexturePaths;

vkr_internal void vkr_material_batch_load_textures(
    VkrMaterialSystem *material_system, VkrAllocator *temp_alloc,
    const VkrMaterialTexturePaths *paths, VkrMaterial *out_material) {
  // Count valid texture paths
  uint32_t count = 0;
  String8 batch_paths[3];
  VkrTextureSlot batch_slots[3];

  if (paths->diffuse.str && paths->diffuse.length > 0) {
    batch_paths[count] = paths->diffuse;
    batch_slots[count] = VKR_TEXTURE_SLOT_DIFFUSE;
    count++;
  }
  if (paths->specular.str && paths->specular.length > 0) {
    batch_paths[count] = paths->specular;
    batch_slots[count] = VKR_TEXTURE_SLOT_SPECULAR;
    count++;
  }
  if (paths->normal.str && paths->normal.length > 0) {
    batch_paths[count] = paths->normal;
    batch_slots[count] = VKR_TEXTURE_SLOT_NORMAL;
    count++;
  }

  if (count == 0) {
    return;
  }

  VkrTextureHandle handles[3] = {VKR_TEXTURE_HANDLE_INVALID,
                                 VKR_TEXTURE_HANDLE_INVALID,
                                 VKR_TEXTURE_HANDLE_INVALID};
  VkrRendererError errors[3] = {VKR_RENDERER_ERROR_NONE,
                                VKR_RENDERER_ERROR_NONE,
                                VKR_RENDERER_ERROR_NONE};

  uint32_t loaded = vkr_texture_system_load_batch(
      material_system->texture_system, batch_paths, count, handles, errors);

  log_debug("Material batch loaded %u/%u textures", loaded, count);

  for (uint32_t i = 0; i < count; i++) {
    VkrTextureSlot slot = batch_slots[i];

    if (handles[i].id != 0) {
      vkr_texture_system_add_ref_by_handle(material_system->texture_system,
                                           handles[i]);
      out_material->textures[slot].handle = handles[i];
      out_material->textures[slot].enabled = true;
    } else if (errors[i] != VKR_RENDERER_ERROR_NONE) {
      String8 err_str = vkr_renderer_get_error_string(errors[i]);
      log_warn("Failed to load texture slot %u: %.*s", slot,
               (int)err_str.length, err_str.str);
    }
  }
}

vkr_internal VkrRendererError vkr_material_loader_load_from_mt(
    VkrResourceLoader *self, String8 path, VkrAllocator *temp_alloc,
    VkrMaterial *out_material) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(temp_alloc != NULL, "Temp allocator is NULL");
  assert_log(out_material != NULL, "Out material is NULL");

  VkrMaterialSystem *material_system =
      (VkrMaterialSystem *)self->resource_system;

  FilePath fp = file_path_create((const char *)path.str, temp_alloc,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle fh = {0};
  FileError fe = file_open(&fp, mode, &fh);
  if (fe != FILE_ERROR_NONE) {
    log_error("Failed to open material file '%s': %s", (char *)path.str,
              file_get_error_string(fe).str);
    return VKR_RENDERER_ERROR_FILE_NOT_FOUND;
  }

  char material_name_buf[VKR_MATERIAL_NAME_MAX] = {0};
  String8 material_name = {0};
  vkr_get_stable_material_name(material_name_buf, path, &material_name);

  vkr_material_init_defaults(out_material, material_system);

  VkrMaterialTexturePaths texture_paths = {0};
  bool8_t alpha_cutoff_set = false_v;

  while (true) {
    String8 line = {0};
    FileError le = file_read_line(&fh, temp_alloc, temp_alloc, 32000, &line);
    if (le == FILE_ERROR_EOF) {
      log_debug("Reached end of material file '%s'", (char *)path.str);
      break;
    }

    if (le != FILE_ERROR_NONE) {
      log_error("Failed reading '%s': %s", (char *)path.str,
                file_get_error_string(le).str);
      break;
    }

    string8_trim(&line);
    if (line.length == 0 || line.str[0] == '#') {
      continue;
    }

    uint64_t eq = 0;
    bool found = false;
    for (uint64_t ch = 0; ch < line.length; ch++) {
      if (line.str[ch] == '=') {
        eq = ch;
        found = true;
        break;
      }
    }

    if (!found || eq == 0 || eq + 1 >= line.length) {
      continue;
    }

    String8 key = string8_substring(&line, 0, eq);
    String8 value = string8_substring(&line, eq + 1, line.length);
    string8_trim(&key);
    string8_trim(&value);

    if (vkr_string8_equals_cstr_i(&key, "name")) {
      if (value.length == 0) {
        log_warn("Material '%s': empty name field ignored",
                 string8_cstr(&material_name));
        continue;
      }

      char *explicit_name = (char *)vkr_allocator_alloc(
          temp_alloc, value.length + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      if (!explicit_name) {
        log_warn("Material '%s': failed to allocate explicit name",
                 string8_cstr(&material_name));
        continue;
      }

      MemCopy(explicit_name, value.str, (size_t)value.length);
      explicit_name[value.length] = '\0';
      out_material->name = explicit_name;
      material_name = string8_create_from_cstr((const uint8_t *)explicit_name,
                                               value.length);

    } else if (string8_contains_cstr(&key, "diffuse_texture")) {
      if (value.length > 0) {
        texture_paths.diffuse = string8_duplicate(temp_alloc, &value);
      }
    } else if (string8_contains_cstr(&key, "specular_texture")) {
      if (value.length > 0) {
        texture_paths.specular = string8_duplicate(temp_alloc, &value);
      }
    } else if (string8_contains_cstr(&key, "norm_texture") ||
               string8_contains_cstr(&key, "normal_texture")) {
      if (value.length > 0) {
        texture_paths.normal = string8_duplicate(temp_alloc, &value);
      }
    } else if (string8_contains_cstr(&key, "diffuse_colorspace")) {
      vkr_material_parse_colorspace_field(key, value, material_name,
                                          &texture_paths.diffuse_colorspace,
                                          "diffuse");
    } else if (string8_contains_cstr(&key, "specular_colorspace")) {
      vkr_material_parse_colorspace_field(key, value, material_name,
                                          &texture_paths.specular_colorspace,
                                          "specular");
    } else if (string8_contains_cstr(&key, "norm_colorspace") ||
               string8_contains_cstr(&key, "normal_colorspace")) {
      vkr_material_parse_colorspace_field(key, value, material_name,
                                          &texture_paths.normal_colorspace,
                                          "normal");
    } else if (string8_contains_cstr(&key, "diffuse_color")) {
      Vec4 v;
      if (string8_to_vec4(&value, &v)) {
        out_material->phong.diffuse_color = v;
      } else {
        log_warn("Material '%s': invalid diffuse_color '%s'",
                 string8_cstr(&material_name), string8_cstr(&value));
      }

    } else if (string8_contains_cstr(&key, "specular_color")) {
      Vec4 v;
      if (string8_to_vec4(&value, &v)) {
        out_material->phong.specular_color = v;
      } else {
        log_warn("Material '%s': invalid specular_color '%s'",
                 string8_cstr(&material_name), string8_cstr(&value));
      }

    } else if (string8_contains_cstr(&key, "shininess")) {
      float32_t s = 0.0f;

      if (string8_to_f32(&value, &s)) {
        out_material->phong.shininess = s;
      } else {
        log_warn("Material '%s': invalid shininess '%s'",
                 string8_cstr(&material_name), string8_cstr(&value));
      }

    } else if (string8_contains_cstr(&key, "emission_color")) {
      Vec3 v;
      if (string8_to_vec3(&value, &v)) {
        out_material->phong.emission_color = v;
      } else {
        log_warn("Material '%s': invalid emission_color '%s'",
                 string8_cstr(&material_name), string8_cstr(&value));
      }

    } else if (string8_contains_cstr(&key, "alpha_cutoff")) {
      float32_t cutoff = 0.0f;
      if (string8_to_f32(&value, &cutoff)) {
        if (cutoff < 0.0f) {
          log_warn("Material '%s': alpha_cutoff < 0 clamped to 0",
                   string8_cstr(&material_name));
          cutoff = 0.0f;
        }
        out_material->alpha_cutoff = cutoff;
        alpha_cutoff_set = true_v;
      } else {
        log_warn("Material '%s': invalid alpha_cutoff '%s'",
                 string8_cstr(&material_name), string8_cstr(&value));
      }

    } else if (string8_contains_cstr(&key, "cutout")) {
      bool8_t cutout = false_v;
      if (string8_to_bool(&value, &cutout)) {
        if (!alpha_cutoff_set) {
          out_material->alpha_cutoff =
              cutout ? VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT : 0.0f;
        }
      } else {
        log_warn("Material '%s': invalid cutout '%s'",
                 string8_cstr(&material_name), string8_cstr(&value));
      }

    } else if (string8_contains_cstr(&key, "shader")) {
      char shader_name[VKR_MATERIAL_NAME_MAX] = {0};
      uint32_t trimmed_len = 0;
      if (!vkr_material_copy_trimmed_value_to_cstr(
              value, shader_name, sizeof(shader_name), &trimmed_len)) {
        continue;
      }
      char *stable = (char *)vkr_allocator_alloc(
          &material_system->string_allocator, trimmed_len + 1,
          VKR_ALLOCATOR_MEMORY_TAG_STRING);
      if (stable) {
        MemCopy(stable, shader_name, (size_t)trimmed_len);
        stable[trimmed_len] = '\0';
        out_material->shader_name = stable;
      } else {
        log_warn("Material '%s': failed to allocate shader name",
                 string8_cstr(&material_name));
      }
    } else if (string8_contains_cstr(&key, "pipeline")) {
      char pipeline_name[VKR_MATERIAL_NAME_MAX] = {0};
      if (!vkr_material_copy_trimmed_value_to_cstr(
              value, pipeline_name, sizeof(pipeline_name), NULL)) {
        continue;
      }
      uint32_t pipeline_id = vkr_get_pipeline_id_from_string(pipeline_name);
      if (pipeline_id != VKR_INVALID_ID) {
        out_material->pipeline_id = pipeline_id;
      } else {
        log_warn("Material '%s': invalid pipeline '%s'",
                 string8_cstr(&material_name), string8_cstr(&value));
        out_material->pipeline_id = VKR_INVALID_ID;
      }
    } else {
      log_debug("Material '%s': ignoring unknown key '%.*s'",
                string8_cstr(&material_name), (int)key.length, (char *)key.str);
    }
  }

  file_close(&fh);

  texture_paths.diffuse = vkr_material_apply_colorspace(
      temp_alloc, texture_paths.diffuse, texture_paths.diffuse_colorspace,
      material_name, "diffuse");
  texture_paths.specular = vkr_material_apply_colorspace(
      temp_alloc, texture_paths.specular, texture_paths.specular_colorspace,
      material_name, "specular");
  texture_paths.normal = vkr_material_apply_colorspace(
      temp_alloc, texture_paths.normal, texture_paths.normal_colorspace,
      material_name, "normal");

  vkr_material_batch_load_textures(material_system, temp_alloc, &texture_paths,
                                   out_material);

  return VKR_RENDERER_ERROR_NONE;
}

vkr_internal uint32_t vkr_material_loader_batch_load_callback(
    VkrResourceLoader *self, const String8 *paths, uint32_t count,
    VkrAllocator *temp_alloc, VkrResourceHandleInfo *out_handles,
    VkrRendererError *out_errors) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  for (uint32_t i = 0; i < count; i++) {
    out_handles[i].type = VKR_RESOURCE_TYPE_UNKNOWN;
    out_handles[i].loader_id = VKR_INVALID_ID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  VkrJobSystem *job_system = vkr_resource_system_get_job_system();

  VkrMaterialHandle *material_handles =
      vkr_allocator_alloc(temp_alloc, sizeof(VkrMaterialHandle) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!material_handles) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }

  VkrMaterialBatchContext batch_ctx = {
      .material_system = (struct VkrMaterialSystem *)self->resource_system,
      .job_system = job_system,
      .allocator = temp_alloc,
      .temp_allocator = temp_alloc,
  };

  uint32_t loaded_count = vkr_material_loader_load_batch(
      &batch_ctx, paths, count, material_handles, out_errors);

  for (uint32_t i = 0; i < count; i++) {
    if (material_handles[i].id != 0) {
      out_handles[i].type = VKR_RESOURCE_TYPE_MATERIAL;
      out_handles[i].loader_id = self->id;
      out_handles[i].as.material = material_handles[i];
    }
  }

  return loaded_count;
}

VkrResourceLoader vkr_material_loader_create(void) {
  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_MATERIAL;
  loader.can_load = vkr_material_loader_can_load;
  loader.load = vkr_material_loader_load;
  loader.prepare_async = vkr_material_loader_prepare_async;
  loader.finalize_async = vkr_material_loader_finalize_async;
  loader.estimate_async_finalize_cost =
      vkr_material_loader_estimate_async_finalize_cost;
  loader.release_async_payload = vkr_material_loader_release_async_payload;
  loader.unload = vkr_material_loader_unload;
  loader.batch_load = vkr_material_loader_batch_load_callback;
  return loader;
}

vkr_internal bool8_t vkr_material_loader_parse_file(
    VkrAllocator *allocator, String8 path, VkrParsedMaterialData *out_data) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_data != NULL, "Out data is NULL");

  MemZero(out_data, sizeof(*out_data));
  out_data->parse_success = false_v;
  out_data->parse_error = VKR_RENDERER_ERROR_NONE;

  out_data->phong.diffuse_color = vec4_new(1, 1, 1, 1);
  out_data->phong.specular_color = vec4_new(1, 1, 1, 1);
  out_data->phong.shininess = 32.0f;
  out_data->phong.emission_color = vec3_new(0, 0, 0);
  out_data->alpha_cutoff = 0.0f;
  out_data->alpha_cutoff_set = false_v;
  out_data->cutout_enabled = false_v;
  out_data->pipeline_id = VKR_INVALID_ID;
  out_data->diffuse_colorspace = VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR;
  out_data->specular_colorspace = VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR;
  out_data->normal_colorspace = VKR_MATERIAL_TEXTURE_COLORSPACE_LINEAR;

  String8 material_name = {0};
  vkr_get_stable_material_name(out_data->name, path, &material_name);

  FilePath fp = file_path_create((const char *)path.str, allocator,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle fh = {0};
  FileError fe = file_open(&fp, mode, &fh);
  if (fe != FILE_ERROR_NONE) {
    out_data->parse_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  String8 file_content = {0};
  FileError read_err = file_read_string(&fh, allocator, &file_content);
  file_close(&fh);

  if (read_err != FILE_ERROR_NONE) {
    out_data->parse_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  uint64_t offset = 0;
  while (offset < file_content.length) {
    uint64_t line_end = offset;
    while (line_end < file_content.length &&
           file_content.str[line_end] != '\n' &&
           file_content.str[line_end] != '\r') {
      line_end++;
    }

    String8 line = string8_substring(&file_content, offset, line_end);

    offset = line_end;
    while (offset < file_content.length && (file_content.str[offset] == '\n' ||
                                            file_content.str[offset] == '\r')) {
      offset++;
    }

    string8_trim(&line);
    if (line.length == 0 || line.str[0] == '#') {
      continue;
    }

    String8 key = {0};
    String8 value = {0};
    if (!vkr_material_parse_line_key_value(line, &key, &value)) {
      continue;
    }

    if (vkr_string8_equals_cstr_i(&key, "name")) {
      if (value.length > 0 && value.length < sizeof(out_data->name)) {
        MemCopy(out_data->name, value.str, (size_t)value.length);
        out_data->name[value.length] = '\0';
      }
    } else if (string8_contains_cstr(&key, "diffuse_texture")) {
      if (value.length > 0 && value.length < VKR_MATERIAL_PATH_MAX) {
        MemCopy(out_data->diffuse_path, value.str, (size_t)value.length);
        out_data->diffuse_path[value.length] = '\0';
      }
    } else if (string8_contains_cstr(&key, "specular_texture")) {
      if (value.length > 0 && value.length < VKR_MATERIAL_PATH_MAX) {
        MemCopy(out_data->specular_path, value.str, (size_t)value.length);
        out_data->specular_path[value.length] = '\0';
      }
    } else if (string8_contains_cstr(&key, "norm_texture") ||
               string8_contains_cstr(&key, "normal_texture")) {
      if (value.length > 0 && value.length < VKR_MATERIAL_PATH_MAX) {
        MemCopy(out_data->normal_path, value.str, (size_t)value.length);
        out_data->normal_path[value.length] = '\0';
      }
    } else if (string8_contains_cstr(&key, "diffuse_colorspace")) {
      String8 material_name_str = string8_create_from_cstr(
          (const uint8_t *)out_data->name, string_length(out_data->name));
      vkr_material_parse_colorspace_field(key, value, material_name_str,
                                          &out_data->diffuse_colorspace,
                                          "diffuse");
    } else if (string8_contains_cstr(&key, "specular_colorspace")) {
      String8 material_name_str = string8_create_from_cstr(
          (const uint8_t *)out_data->name, string_length(out_data->name));
      vkr_material_parse_colorspace_field(key, value, material_name_str,
                                          &out_data->specular_colorspace,
                                          "specular");
    } else if (string8_contains_cstr(&key, "norm_colorspace") ||
               string8_contains_cstr(&key, "normal_colorspace")) {
      String8 material_name_str = string8_create_from_cstr(
          (const uint8_t *)out_data->name, string_length(out_data->name));
      vkr_material_parse_colorspace_field(key, value, material_name_str,
                                          &out_data->normal_colorspace,
                                          "normal");
    } else if (string8_contains_cstr(&key, "diffuse_color")) {
      Vec4 v;
      if (string8_to_vec4(&value, &v)) {
        out_data->phong.diffuse_color = v;
      }
    } else if (string8_contains_cstr(&key, "specular_color")) {
      Vec4 v;
      if (string8_to_vec4(&value, &v)) {
        out_data->phong.specular_color = v;
      }
    } else if (string8_contains_cstr(&key, "shininess")) {
      float32_t s = 0.0f;
      if (string8_to_f32(&value, &s)) {
        out_data->phong.shininess = s;
      }
    } else if (string8_contains_cstr(&key, "emission_color")) {
      Vec3 v;
      if (string8_to_vec3(&value, &v)) {
        out_data->phong.emission_color = v;
      }
    } else if (string8_contains_cstr(&key, "alpha_cutoff")) {
      float32_t cutoff = 0.0f;
      if (string8_to_f32(&value, &cutoff)) {
        if (cutoff < 0.0f) {
          cutoff = 0.0f;
        }
        out_data->alpha_cutoff = cutoff;
        out_data->alpha_cutoff_set = true_v;
      }
    } else if (string8_contains_cstr(&key, "cutout")) {
      bool8_t cutout = false_v;
      if (string8_to_bool(&value, &cutout)) {
        out_data->cutout_enabled = cutout;
      }
    } else if (string8_contains_cstr(&key, "shader")) {
      char shader_name[VKR_MATERIAL_NAME_MAX] = {0};
      uint32_t trimmed_len = 0;
      if (!vkr_material_copy_trimmed_value_to_cstr(
              value, shader_name, sizeof(shader_name), &trimmed_len)) {
        continue;
      }
      if (trimmed_len > 0 && trimmed_len < sizeof(out_data->shader_name)) {
        MemCopy(out_data->shader_name, shader_name, (size_t)trimmed_len);
        out_data->shader_name[trimmed_len] = '\0';
      }
    } else if (string8_contains_cstr(&key, "pipeline")) {
      char pipeline_name[VKR_MATERIAL_NAME_MAX] = {0};
      if (!vkr_material_copy_trimmed_value_to_cstr(
              value, pipeline_name, sizeof(pipeline_name), NULL)) {
        continue;
      }
      out_data->pipeline_id = vkr_get_pipeline_id_from_string(pipeline_name);
    }
  }

  out_data->parse_success = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_material_parse_job_run(VkrJobContext *ctx,
                                                void *payload) {
  VkrMaterialParseJobPayload *job = (VkrMaterialParseJobPayload *)payload;
  String8 path = string8_create_from_cstr((const uint8_t *)job->file_path,
                                          string_length(job->file_path));
  return vkr_material_loader_parse_file(ctx->allocator, path, job->result);
}

vkr_internal uint32_t vkr_material_loader_load_batch(
    VkrMaterialBatchContext *context, const String8 *material_paths,
    uint32_t count, VkrMaterialHandle *out_handles,
    VkrRendererError *out_errors) {
  assert_log(context != NULL, "Context is NULL");
  assert_log(context->material_system != NULL, "Material system is NULL");
  assert_log(material_paths != NULL, "Material paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  if (count == 0) {
    return 0;
  }

  VkrMaterialSystem *mat_sys = context->material_system;
  VkrJobSystem *job_sys = context->job_system;
  (void)job_sys;
  VkrAllocatorScope scratch_scope =
      vkr_allocator_begin_scope(context->temp_allocator);
  if (!vkr_allocator_scope_is_valid(&scratch_scope)) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }

  for (uint32_t i = 0; i < count; i++) {
    out_handles[i] = VKR_MATERIAL_HANDLE_INVALID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  VkrParsedMaterialData *parsed_data = vkr_allocator_alloc(
      context->temp_allocator, sizeof(VkrParsedMaterialData) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!parsed_data) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return 0;
  }

  uint32_t *first_occurrence =
      vkr_allocator_alloc(context->temp_allocator, sizeof(uint32_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!first_occurrence) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return 0;
  }

  for (uint32_t i = 0; i < count; i++) {
    MemZero(&parsed_data[i], sizeof(VkrParsedMaterialData));
    first_occurrence[i] = i;
  }

  uint32_t unique_count = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!material_paths[i].str || material_paths[i].length == 0) {
      continue;
    }

    bool8_t is_duplicate = false_v;
    for (uint32_t j = 0; j < i; j++) {
      if (!material_paths[j].str || material_paths[j].length == 0) {
        continue;
      }
      if (string8_equalsi(&material_paths[i], &material_paths[j])) {
        first_occurrence[i] = first_occurrence[j];
        is_duplicate = true_v;
        break;
      }
    }
    if (!is_duplicate) {
      unique_count++;
    }
  }

  log_debug("Material batch: %u paths, %u unique", count, unique_count);

  for (uint32_t i = 0; i < count; i++) {
    if (!material_paths[i].str || material_paths[i].length == 0 ||
        first_occurrence[i] != i) {
      continue;
    }

    String8 mat_name =
        string8_get_stem(context->temp_allocator, material_paths[i]);
    if (mat_name.str) {
      VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
      VkrMaterialHandle existing =
          vkr_material_system_acquire(mat_sys, mat_name, true_v, &acquire_err);
      if (existing.id != 0) {
        out_handles[i] = existing;
        continue;
      }
    }

    VkrAllocatorScope parse_scope =
        vkr_allocator_begin_scope(context->temp_allocator);
    if (!vkr_allocator_scope_is_valid(&parse_scope)) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      continue;
    }
    vkr_material_loader_parse_file(context->temp_allocator, material_paths[i],
                                   &parsed_data[i]);
    vkr_allocator_end_scope(&parse_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  for (uint32_t i = 0; i < count; i++) {
    if (first_occurrence[i] != i && out_handles[first_occurrence[i]].id != 0) {
      out_handles[i] = out_handles[first_occurrence[i]];
    }
  }

  uint32_t total_textures = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (out_handles[i].id != 0 || !parsed_data[i].parse_success) {
      continue;
    }
    if (parsed_data[i].diffuse_path[0] != '\0') {
      total_textures++;
    }
    if (parsed_data[i].specular_path[0] != '\0') {
      total_textures++;
    }
    if (parsed_data[i].normal_path[0] != '\0') {
      total_textures++;
    }
  }

  String8 *texture_paths = NULL;
  VkrTextureHandle *texture_handles = NULL;
  VkrRendererError *texture_errors = NULL;
  uint32_t *texture_material_index = NULL;
  uint32_t *texture_slot = NULL;

  if (total_textures > 0) {
    texture_paths = vkr_allocator_alloc(context->temp_allocator,
                                        sizeof(String8) * total_textures,
                                        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    texture_handles = vkr_allocator_alloc(
        context->temp_allocator, sizeof(VkrTextureHandle) * total_textures,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    texture_errors = vkr_allocator_alloc(
        context->temp_allocator, sizeof(VkrRendererError) * total_textures,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    texture_material_index = vkr_allocator_alloc(
        context->temp_allocator, sizeof(uint32_t) * total_textures,
        VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    texture_slot = vkr_allocator_alloc(context->temp_allocator,
                                       sizeof(uint32_t) * total_textures,
                                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

    if (!texture_paths || !texture_handles || !texture_errors ||
        !texture_material_index || !texture_slot) {
      for (uint32_t i = 0; i < count; i++) {
        if (out_handles[i].id == 0) {
          out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
        }
      }
      vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      return 0;
    }

    uint32_t tex_idx = 0;
    for (uint32_t i = 0; i < count; i++) {
      if (out_handles[i].id != 0 || !parsed_data[i].parse_success) {
        continue;
      }

      String8 material_name =
          string8_create_from_cstr((const uint8_t *)parsed_data[i].name,
                                   string_length(parsed_data[i].name));

      if (parsed_data[i].diffuse_path[0] != '\0') {
        String8 raw_path = string8_create_from_cstr(
            (const uint8_t *)parsed_data[i].diffuse_path,
            string_length(parsed_data[i].diffuse_path));
        texture_paths[tex_idx] = vkr_material_apply_colorspace(
            context->temp_allocator, raw_path,
            parsed_data[i].diffuse_colorspace, material_name, "diffuse");
        texture_material_index[tex_idx] = i;
        texture_slot[tex_idx] = VKR_TEXTURE_SLOT_DIFFUSE;
        tex_idx++;
      }
      if (parsed_data[i].specular_path[0] != '\0') {
        String8 raw_path = string8_create_from_cstr(
            (const uint8_t *)parsed_data[i].specular_path,
            string_length(parsed_data[i].specular_path));
        texture_paths[tex_idx] = vkr_material_apply_colorspace(
            context->temp_allocator, raw_path,
            parsed_data[i].specular_colorspace, material_name, "specular");
        texture_material_index[tex_idx] = i;
        texture_slot[tex_idx] = VKR_TEXTURE_SLOT_SPECULAR;
        tex_idx++;
      }
      if (parsed_data[i].normal_path[0] != '\0') {
        String8 raw_path = string8_create_from_cstr(
            (const uint8_t *)parsed_data[i].normal_path,
            string_length(parsed_data[i].normal_path));
        texture_paths[tex_idx] = vkr_material_apply_colorspace(
            context->temp_allocator, raw_path, parsed_data[i].normal_colorspace,
            material_name, "normal");
        texture_material_index[tex_idx] = i;
        texture_slot[tex_idx] = VKR_TEXTURE_SLOT_NORMAL;
        tex_idx++;
      }
    }

    uint32_t textures_loaded = vkr_texture_system_load_batch(
        mat_sys->texture_system, texture_paths, total_textures, texture_handles,
        texture_errors);

    log_debug("Material batch loaded %u/%u textures for %u materials",
              textures_loaded, total_textures, count);
  }

  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (out_handles[i].id != 0) {
      loaded++;
      continue;
    }

    if (!parsed_data[i].parse_success) {
      out_errors[i] = parsed_data[i].parse_error;
      continue;
    }

    uint32_t slot = vkr_material_find_slot(mat_sys);
    if (slot == VKR_INVALID_ID) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      continue;
    }

    size_t name_len = string_length(parsed_data[i].name);
    char *stable_name =
        vkr_allocator_alloc(&mat_sys->string_allocator, name_len + 1,
                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!stable_name) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      continue;
    }
    MemCopy(stable_name, parsed_data[i].name, name_len);
    stable_name[name_len] = '\0';

    VkrMaterial *material = &mat_sys->materials.data[slot];
    vkr_material_init_defaults(material, mat_sys);
    material->id = slot + 1;
    material->generation = mat_sys->generation_counter++;
    material->name = stable_name;
    material->pipeline_id = parsed_data[i].pipeline_id;
    material->phong = parsed_data[i].phong;
    if (parsed_data[i].alpha_cutoff_set) {
      material->alpha_cutoff = parsed_data[i].alpha_cutoff;
    } else if (parsed_data[i].cutout_enabled) {
      material->alpha_cutoff = VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT;
    }

    if (parsed_data[i].shader_name[0] != '\0') {
      size_t shader_len = string_length(parsed_data[i].shader_name);
      char *stable_shader =
          vkr_allocator_alloc(&mat_sys->string_allocator, shader_len + 1,
                              VKR_ALLOCATOR_MEMORY_TAG_STRING);
      if (stable_shader) {
        MemCopy(stable_shader, parsed_data[i].shader_name, shader_len);
        stable_shader[shader_len] = '\0';
        material->shader_name = stable_shader;
      }
    }

    for (uint32_t t = 0; t < total_textures; t++) {
      if (texture_material_index[t] == i && texture_handles[t].id != 0) {
        VkrTextureSlot slot_type = (VkrTextureSlot)texture_slot[t];
        vkr_texture_system_add_ref_by_handle(mat_sys->texture_system,
                                             texture_handles[t]);
        material->textures[slot_type].handle = texture_handles[t];
        material->textures[slot_type].enabled = true;
      }
    }

    VkrMaterialEntry new_entry = {
        .id = slot,
        .ref_count = 0,
        .auto_release = true_v,
        .name = stable_name,
    };
    vkr_hash_table_insert_VkrMaterialEntry(&mat_sys->material_by_name,
                                           stable_name, new_entry);

    out_handles[i] = (VkrMaterialHandle){
        .id = material->id,
        .generation = material->generation,
    };
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
    loaded++;
  }

  for (uint32_t i = 0; i < count; i++) {
    if (first_occurrence[i] != i) {
      uint32_t first = first_occurrence[i];
      if (out_handles[first].id != 0) {
        out_handles[i] = out_handles[first];
        out_errors[i] = VKR_RENDERER_ERROR_NONE;
      }
    }
  }

  vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  log_debug("Material batch completed: %u/%u materials loaded (from %u unique)",
            loaded, count, unique_count);
  return loaded;
}
