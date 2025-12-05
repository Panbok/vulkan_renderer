#include "renderer/resources/loaders/material_loader.h"
#include "containers/str.h"
#include "renderer/systems/vkr_material_system.h"

#define VKR_MATERIAL_EXTENSION "mt"

/**
 * @brief Maximum path length for texture paths in parsed material data.
 */
#define VKR_MATERIAL_PATH_MAX 512

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
  char name[128];
  char shader_name[128];
  uint32_t pipeline_id;
  VkrPhongProperties phong;

  // Texture paths as fixed buffers (thread-safe for parallel parsing)
  char diffuse_path[VKR_MATERIAL_PATH_MAX];
  char specular_path[VKR_MATERIAL_PATH_MAX];
  char normal_path[VKR_MATERIAL_PATH_MAX];

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

vkr_internal uint32_t vkr_material_loader_load_batch(
    VkrMaterialBatchContext *context, const String8 *material_paths,
    uint32_t count, VkrMaterialHandle *out_handles,
    VkrRendererError *out_errors);

vkr_internal void vkr_get_stable_material_name(char *material_name_buf,
                                               size_t material_name_buf_size,
                                               String8 name,
                                               String8 *out_name) {
  assert_log(material_name_buf != NULL, "Material name buffer is NULL");
  assert_log(material_name_buf_size > 0, "Material name buffer size is 0");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(out_name != NULL, "Out name is NULL");

  const char *cpath = (const char *)name.str;
  const char *last_slash = strrchr(cpath, '/');
  const char *fname = last_slash ? last_slash + 1 : cpath;
  const char *dot = strrchr(fname, '.');
  if (dot && (size_t)(dot - fname) < material_name_buf_size) {
    MemCopy(material_name_buf, fname, (size_t)(dot - fname));
    material_name_buf[dot - fname] = '\0';
  } else {
    size_t flen = string_length(fname);
    size_t copy_len =
        flen < material_name_buf_size - 1 ? flen : material_name_buf_size - 1;
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

// Forward declarations
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

vkr_internal bool8_t vkr_material_loader_load(VkrResourceLoader *self,
                                              String8 name,
                                              VkrAllocator *temp_alloc,
                                              VkrResourceHandleInfo *out_handle,
                                              VkrRendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_alloc != NULL, "Temp arena is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrMaterialSystem *system = (VkrMaterialSystem *)self->resource_system;

  // Load material from .mt file
  VkrMaterial loaded_material = {0};
  *out_error = vkr_material_loader_load_from_mt(self, name, temp_alloc,
                                                &loaded_material);
  if (*out_error != VKR_RENDERER_ERROR_NONE) {
    return false_v;
  }

  char material_name_buf[128] = {0};
  String8 material_name = {0};
  if (loaded_material.name) {
    uint64_t parsed_name_length = string_length(loaded_material.name);
    if (parsed_name_length > 0) {
      material_name = string8_create_from_cstr(
          (const uint8_t *)loaded_material.name, parsed_name_length);
    }
  }

  if (material_name.length == 0 || material_name.str == NULL) {
    vkr_get_stable_material_name(material_name_buf, 128, name, &material_name);
  }

  // Check if material already exists
  const char *material_key = (const char *)material_name.str;
  VkrMaterialEntry *existing_entry = vkr_hash_table_get_VkrMaterialEntry(
      &system->material_by_name, material_key);
  if (existing_entry) {
    log_warn("Material '%s' already exists in system", material_key);
    *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    return false_v;
  }

  // Find slot
  uint32_t slot = VKR_INVALID_ID;
  if (system->free_count > 0) {
    slot = system->free_ids.data[system->free_count - 1];
    system->free_count--;
  } else {
    // linear probe for empty slot
    slot = system->next_free_index;
    while (slot < system->materials.length &&
           system->materials.data[slot].id != 0)
      slot++;
    if (slot >= system->materials.length) {
      log_error("Material system is full");
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }
    system->next_free_index = slot + 1;
  }

  // Store stable copy of key in system arena
  char *stable_name =
      vkr_allocator_alloc(&system->allocator, material_name.length + 1,
                          VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!stable_name) {
    log_error("Failed to allocate name for material");
    *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return false_v;
  }
  MemCopy(stable_name, material_name.str, (size_t)material_name.length);
  stable_name[material_name.length] = '\0';

  // Copy material data to system
  VkrMaterial *material = &system->materials.data[slot];
  *material = loaded_material;
  material->name = (const char *)stable_name;

  // Assign stable id and generation
  material->id = slot + 1;
  material->generation = system->generation_counter++;

  // Add to hash table with 0 ref count
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

vkr_internal void
vkr_material_loader_unload(VkrResourceLoader *self,
                           const VkrResourceHandleInfo *handle, String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(handle != NULL, "Handle is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  VkrMaterialSystem *system = (VkrMaterialSystem *)self->resource_system;

  char material_name_buf[128] = {0};
  String8 material_name = {0};
  vkr_get_stable_material_name(material_name_buf, 128, name, &material_name);

  const char *material_key = (const char *)material_name.str;
  VkrMaterialEntry *entry = vkr_hash_table_get_VkrMaterialEntry(
      &system->material_by_name, material_key);

  if (!entry) {
    log_warn("Attempted to remove unknown material '%s'", material_key);
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

  for (uint32_t tex_slot = 0; tex_slot < VKR_TEXTURE_SLOT_COUNT; tex_slot++) {
    VkrTextureHandle handle = material->textures[tex_slot].handle;
    if (handle.id != 0) {
      vkr_texture_system_release_by_handle(system->texture_system, handle);
    }
  }

  material->id = 0;
  material->name = NULL;
  material->pipeline_id = VKR_INVALID_ID;
  material->phong.diffuse_color = vec4_new(1, 1, 1, 1);
  material->phong.specular_color = vec4_new(1, 1, 1, 1);
  material->phong.shininess = 0.0f;
  material->phong.emission_color = vec3_new(0, 0, 0);
  for (uint32_t tex_slot = 0; tex_slot < VKR_TEXTURE_SLOT_COUNT; tex_slot++) {
    material->textures[tex_slot].handle = VKR_TEXTURE_HANDLE_INVALID;
    material->textures[tex_slot].enabled = false;
    material->textures[tex_slot].slot = (VkrTextureSlot)tex_slot;
  }

  // Add to free list
  assert_log(system->free_count < system->free_ids.length,
             "free_ids overflow in material system");
  system->free_ids.data[system->free_count++] = material_index;

  // Remove from hash table
  vkr_hash_table_remove_VkrMaterialEntry(&system->material_by_name,
                                         material_key);

  if (material_index < system->next_free_index) {
    system->next_free_index = material_index;
  }
}

// Structure to hold pending texture paths for batch loading
typedef struct VkrMaterialTexturePaths {
  String8 diffuse;
  String8 specular;
  String8 normal;
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

  // Batch load all textures in parallel
  uint32_t loaded = vkr_texture_system_load_batch(
      material_system->texture_system, batch_paths, count, handles, errors);

  log_debug("Material batch loaded %u/%u textures", loaded, count);

  // Acquire and assign handles
  for (uint32_t i = 0; i < count; i++) {
    VkrTextureSlot slot = batch_slots[i];

    if (handles[i].id != 0) {
      VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
      VkrTextureHandle acquired =
          vkr_texture_system_acquire(material_system->texture_system,
                                     batch_paths[i], true_v, &acquire_err);
      if (acquired.id != 0) {
        out_material->textures[slot].handle = acquired;
        out_material->textures[slot].enabled = true;
      }
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
  assert_log(temp_alloc != NULL, "Temp arena is NULL");
  assert_log(out_material != NULL, "Out material is NULL");

  VkrMaterialSystem *material_system =
      (VkrMaterialSystem *)self->resource_system;
  ;
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

  char material_name_buf[128] = {0};
  String8 material_name = {0};
  vkr_get_stable_material_name(material_name_buf, 128, path, &material_name);

  // Initialize material with defaults
  MemZero(out_material, sizeof(*out_material));
  out_material->phong.diffuse_color = vec4_new(1, 1, 1, 1);
  out_material->phong.specular_color = vec4_new(1, 1, 1, 1);
  out_material->phong.shininess = 32.0f;
  out_material->phong.emission_color = vec3_new(0, 0, 0);

  // Initialize all texture slots disabled
  for (uint32_t tex_slot = 0; tex_slot < VKR_TEXTURE_SLOT_COUNT; tex_slot++) {
    out_material->textures[tex_slot].slot = (VkrTextureSlot)tex_slot;
    out_material->textures[tex_slot].handle = VKR_TEXTURE_HANDLE_INVALID;
    out_material->textures[tex_slot].enabled = false;
  }

  out_material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      vkr_texture_system_get_default_handle(material_system->texture_system);
  out_material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true;
  out_material->textures[VKR_TEXTURE_SLOT_DIFFUSE].slot =
      VKR_TEXTURE_SLOT_DIFFUSE;
  out_material->textures[VKR_TEXTURE_SLOT_NORMAL].handle =
      vkr_texture_system_get_default_normal_handle(
          material_system->texture_system);
  out_material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled = true;
  out_material->textures[VKR_TEXTURE_SLOT_NORMAL].slot =
      VKR_TEXTURE_SLOT_NORMAL;
  out_material->textures[VKR_TEXTURE_SLOT_SPECULAR].handle =
      vkr_texture_system_get_default_specular_handle(
          material_system->texture_system);
  out_material->textures[VKR_TEXTURE_SLOT_SPECULAR].enabled = true;
  out_material->textures[VKR_TEXTURE_SLOT_SPECULAR].slot =
      VKR_TEXTURE_SLOT_SPECULAR;

  // Collect texture paths for batch loading
  VkrMaterialTexturePaths texture_paths = {0};

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

    // Find '='
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
      // Collect path for batch loading
      if (value.length > 0) {
        texture_paths.diffuse = string8_duplicate(temp_alloc, &value);
      }
    } else if (string8_contains_cstr(&key, "specular_texture")) {
      // Collect path for batch loading
      if (value.length > 0) {
        texture_paths.specular = string8_duplicate(temp_alloc, &value);
      }
    } else if (string8_contains_cstr(&key, "norm_texture") ||
               string8_contains_cstr(&key, "normal_texture")) {
      // Collect path for batch loading
      if (value.length > 0) {
        texture_paths.normal = string8_duplicate(temp_alloc, &value);
      }
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

    } else if (string8_contains_cstr(&key, "shader")) {
      // Preferred shader name (e.g., shader.default.world)
      // Trim whitespace/newlines from the value before storing
      char *trimmed = (char *)value.str;
      trimmed = string_trim(trimmed);
      uint32_t trimmed_len = (uint32_t)string_length(trimmed);
      VkrAllocator mat_alloc = {.ctx = material_system->arena};
      vkr_allocator_arena(&mat_alloc);
      char *stable = (char *)vkr_allocator_alloc(
          &mat_alloc, trimmed_len + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      if (stable) {
        MemCopy(stable, trimmed, (size_t)trimmed_len);
        stable[trimmed_len] = '\0';
        out_material->shader_name = stable;
      } else {
        log_warn("Material '%s': failed to allocate shader name",
                 string8_cstr(&material_name));
      }
    } else if (string8_contains_cstr(&key, "pipeline")) {
      string_trim((char *)value.str);
      uint32_t pipeline_id = vkr_get_pipeline_id_from_string((char *)value.str);
      if (pipeline_id != VKR_INVALID_ID) {
        out_material->pipeline_id = pipeline_id;
      } else {
        log_warn("Material '%s': invalid pipeline '%s'",
                 string8_cstr(&material_name), string8_cstr(&value));
        out_material->pipeline_id = VKR_INVALID_ID;
      }
    } else {
      // Unknown keys are ignored for now
      log_debug("Material '%s': ignoring unknown key '%.*s'",
                string8_cstr(&material_name), (int)key.length, (char *)key.str);
    }
  }

  file_close(&fh);

  // Batch load all collected textures in parallel
  vkr_material_batch_load_textures(material_system, temp_alloc, &texture_paths,
                                   out_material);

  return VKR_RENDERER_ERROR_NONE;
}

// Batch load callback - uses job system from resource system for parallel
// loading
vkr_internal uint32_t vkr_material_loader_batch_load_callback(
    VkrResourceLoader *self, const String8 *paths, uint32_t count,
    VkrAllocator *temp_alloc, VkrResourceHandleInfo *out_handles,
    VkrRendererError *out_errors) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(paths != NULL, "Paths is NULL");
  assert_log(out_handles != NULL, "Out handles is NULL");
  assert_log(out_errors != NULL, "Out errors is NULL");

  // Initialize outputs
  for (uint32_t i = 0; i < count; i++) {
    out_handles[i].type = VKR_RESOURCE_TYPE_UNKNOWN;
    out_handles[i].loader_id = VKR_INVALID_ID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  // Get job system from resource system for parallel loading
  VkrJobSystem *job_system = vkr_resource_system_get_job_system();

  // Allocate material handles array for vkr_material_loader_load_batch
  VkrMaterialHandle *material_handles =
      vkr_allocator_alloc(temp_alloc, sizeof(VkrMaterialHandle) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!material_handles) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }

  // Use parallel batch loading
  VkrMaterialBatchContext batch_ctx = {
      .material_system = (struct VkrMaterialSystem *)self->resource_system,
      .job_system = job_system,
      .allocator = temp_alloc,
      .temp_allocator = temp_alloc,
  };

  uint32_t loaded_count = vkr_material_loader_load_batch(
      &batch_ctx, paths, count, material_handles, out_errors);

  // Convert material handles to resource handle infos
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
  loader.unload = vkr_material_loader_unload;
  loader.batch_load = vkr_material_loader_batch_load_callback;
  return loader;
}

// =============================================================================
// Batch Material Loading Implementation
// =============================================================================

vkr_internal bool8_t vkr_material_loader_parse_file(
    VkrAllocator *allocator, String8 path, VkrParsedMaterialData *out_data) {
  assert_log(allocator != NULL, "Allocator is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(out_data != NULL, "Out data is NULL");

  MemZero(out_data, sizeof(*out_data));
  out_data->parse_success = false_v;
  out_data->parse_error = VKR_RENDERER_ERROR_NONE;

  // Default values
  out_data->phong.diffuse_color = vec4_new(1, 1, 1, 1);
  out_data->phong.specular_color = vec4_new(1, 1, 1, 1);
  out_data->phong.shininess = 32.0f;
  out_data->phong.emission_color = vec3_new(0, 0, 0);
  out_data->pipeline_id = VKR_INVALID_ID;

  // Extract name from path
  const char *cpath = (const char *)path.str;
  const char *last_slash = strrchr(cpath, '/');
  const char *fname = last_slash ? last_slash + 1 : cpath;
  const char *dot = strrchr(fname, '.');
  if (dot && (size_t)(dot - fname) < sizeof(out_data->name)) {
    MemCopy(out_data->name, fname, (size_t)(dot - fname));
    out_data->name[dot - fname] = '\0';
  } else {
    size_t flen = string_length(fname);
    size_t copy_len =
        flen < sizeof(out_data->name) - 1 ? flen : sizeof(out_data->name) - 1;
    MemCopy(out_data->name, fname, copy_len);
    out_data->name[copy_len] = '\0';
  }

  // Open file
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

  // Read entire file
  String8 file_content = {0};
  FileError read_err = file_read_string(&fh, allocator, &file_content);
  file_close(&fh);

  if (read_err != FILE_ERROR_NONE) {
    out_data->parse_error = VKR_RENDERER_ERROR_FILE_NOT_FOUND;
    return false_v;
  }

  // Parse line by line
  uint64_t offset = 0;
  while (offset < file_content.length) {
    // Find line end
    uint64_t line_end = offset;
    while (line_end < file_content.length &&
           file_content.str[line_end] != '\n' &&
           file_content.str[line_end] != '\r') {
      line_end++;
    }

    String8 line = string8_substring(&file_content, offset, line_end);

    // Skip to next line
    offset = line_end;
    while (offset < file_content.length && (file_content.str[offset] == '\n' ||
                                            file_content.str[offset] == '\r')) {
      offset++;
    }

    string8_trim(&line);
    if (line.length == 0 || line.str[0] == '#') {
      continue;
    }

    // Find '='
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
    } else if (string8_contains_cstr(&key, "shader")) {
      char *trimmed = (char *)value.str;
      trimmed = string_trim(trimmed);
      uint32_t trimmed_len = (uint32_t)string_length(trimmed);
      if (trimmed_len > 0 && trimmed_len < sizeof(out_data->shader_name)) {
        MemCopy(out_data->shader_name, trimmed, (size_t)trimmed_len);
        out_data->shader_name[trimmed_len] = '\0';
      }
    } else if (string8_contains_cstr(&key, "pipeline")) {
      char *trimmed = string_trim((char *)value.str);
      out_data->pipeline_id = vkr_get_pipeline_id_from_string(trimmed);
    }
  }

  out_data->parse_success = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_material_parse_job_run(VkrJobContext *ctx,
                                                void *payload) {
  VkrMaterialParseJobPayload *job = (VkrMaterialParseJobPayload *)payload;
  // Use the job context's thread-local scratch arena for internal allocations
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
  VkrAllocatorScope scratch_scope =
      vkr_allocator_begin_scope(context->temp_allocator);
  if (!vkr_allocator_scope_is_valid(&scratch_scope)) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    return 0;
  }

  // Initialize outputs
  for (uint32_t i = 0; i < count; i++) {
    out_handles[i] = VKR_MATERIAL_HANDLE_INVALID;
    out_errors[i] = VKR_RENDERER_ERROR_NONE;
  }

  // Allocate arrays for parsed data and job handles
  VkrParsedMaterialData *parsed_data = vkr_allocator_alloc(
      context->temp_allocator, sizeof(VkrParsedMaterialData) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrJobHandle *job_handles =
      vkr_allocator_alloc(context->temp_allocator, sizeof(VkrJobHandle) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  VkrMaterialParseJobPayload *payloads = vkr_allocator_alloc(
      context->temp_allocator, sizeof(VkrMaterialParseJobPayload) * count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  bool8_t *job_submitted =
      vkr_allocator_alloc(context->temp_allocator, sizeof(bool8_t) * count,
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);

  if (!parsed_data || !job_handles || !payloads || !job_submitted) {
    for (uint32_t i = 0; i < count; i++) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    }
    vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    return 0;
  }

  // Initialize parsed data and deduplication mapping
  // first_occurrence[i] = index of first occurrence of this material path, or i
  // if unique
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
    job_submitted[i] = false_v;
    first_occurrence[i] = i; // Default: each is its own first occurrence
  }

  // Deduplicate material paths within the batch
  uint32_t unique_count = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (!material_paths[i].str || material_paths[i].length == 0) {
      continue;
    }

    // Check if this path appeared earlier in the batch
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

  // Submit parse jobs only for unique material paths
  if (job_sys) {
    Bitset8 type_mask = bitset8_create();
    bitset8_set(&type_mask, VKR_JOB_TYPE_RESOURCE);

    for (uint32_t i = 0; i < count; i++) {
      if (!material_paths[i].str || material_paths[i].length == 0) {
        continue;
      }

      // Skip duplicates - they'll get their handle from first occurrence later
      if (first_occurrence[i] != i) {
        continue;
      }

      String8 mat_name =
          string8_get_stem(context->temp_allocator, material_paths[i]);
      if (mat_name.str) {
        VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
        VkrMaterialHandle existing = vkr_material_system_acquire(
            mat_sys, mat_name, true_v, &acquire_err);
        if (existing.id != 0) {
          out_handles[i] = existing;
          continue;
        }
      }

      // Copy path to fixed buffer in payload (thread-safe)
      MemZero(&payloads[i], sizeof(VkrMaterialParseJobPayload));
      uint64_t path_len = material_paths[i].length < VKR_MATERIAL_PATH_MAX - 1
                              ? material_paths[i].length
                              : VKR_MATERIAL_PATH_MAX - 1;
      MemCopy(payloads[i].file_path, material_paths[i].str, (size_t)path_len);
      payloads[i].file_path[path_len] = '\0';
      payloads[i].result = &parsed_data[i];

      VkrJobDesc job_desc = {
          .priority = VKR_JOB_PRIORITY_NORMAL,
          .type_mask = type_mask,
          .run = vkr_material_parse_job_run,
          .on_success = NULL,
          .on_failure = NULL,
          .payload = &payloads[i],
          .payload_size = sizeof(VkrMaterialParseJobPayload),
          .dependencies = NULL,
          .dependency_count = 0,
          .defer_enqueue = false_v,
      };

      if (vkr_job_submit(job_sys, &job_desc, &job_handles[i])) {
        job_submitted[i] = true_v;
      }
    }

    // Wait for all parse jobs to complete
    for (uint32_t i = 0; i < count; i++) {
      if (job_submitted[i]) {
        vkr_job_wait(job_sys, job_handles[i]);
      }
    }
  } else {
    // Synchronous fallback - each parse uses its own scratch
    for (uint32_t i = 0; i < count; i++) {
      if (!material_paths[i].str || material_paths[i].length == 0) {
        continue;
      }

      // Skip duplicates
      if (first_occurrence[i] != i) {
        continue;
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
  }

  // Copy handles from first occurrence to duplicates
  for (uint32_t i = 0; i < count; i++) {
    if (first_occurrence[i] != i && out_handles[first_occurrence[i]].id != 0) {
      out_handles[i] = out_handles[first_occurrence[i]];
    }
  }

  // Collect ALL texture paths from all parsed materials
  uint32_t total_textures = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (out_handles[i].id != 0)
      continue; // Already loaded
    if (!parsed_data[i].parse_success)
      continue;

    if (parsed_data[i].diffuse_path[0] != '\0')
      total_textures++;
    if (parsed_data[i].specular_path[0] != '\0')
      total_textures++;
    if (parsed_data[i].normal_path[0] != '\0')
      total_textures++;
  }

  String8 *texture_paths = NULL;
  VkrTextureHandle *texture_handles = NULL;
  VkrRendererError *texture_errors = NULL;
  uint32_t *texture_material_index =
      NULL;                      // Which material this texture belongs to
  uint32_t *texture_slot = NULL; // Which slot (diffuse/spec/normal)

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

    // Collect all texture paths (convert fixed buffers to String8)
    uint32_t tex_idx = 0;
    for (uint32_t i = 0; i < count; i++) {
      if (out_handles[i].id != 0)
        continue;
      if (!parsed_data[i].parse_success)
        continue;

      if (parsed_data[i].diffuse_path[0] != '\0') {
        texture_paths[tex_idx] = string8_create_from_cstr(
            (const uint8_t *)parsed_data[i].diffuse_path,
            string_length(parsed_data[i].diffuse_path));
        texture_material_index[tex_idx] = i;
        texture_slot[tex_idx] = VKR_TEXTURE_SLOT_DIFFUSE;
        tex_idx++;
      }
      if (parsed_data[i].specular_path[0] != '\0') {
        texture_paths[tex_idx] = string8_create_from_cstr(
            (const uint8_t *)parsed_data[i].specular_path,
            string_length(parsed_data[i].specular_path));
        texture_material_index[tex_idx] = i;
        texture_slot[tex_idx] = VKR_TEXTURE_SLOT_SPECULAR;
        tex_idx++;
      }
      if (parsed_data[i].normal_path[0] != '\0') {
        texture_paths[tex_idx] = string8_create_from_cstr(
            (const uint8_t *)parsed_data[i].normal_path,
            string_length(parsed_data[i].normal_path));
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

  // Create materials and assign texture handles
  uint32_t loaded = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (out_handles[i].id != 0) {
      loaded++; // Already existed
      continue;
    }

    if (!parsed_data[i].parse_success) {
      out_errors[i] = parsed_data[i].parse_error;
      continue;
    }

    uint32_t slot = VKR_INVALID_ID;
    if (mat_sys->free_count > 0) {
      slot = mat_sys->free_ids.data[mat_sys->free_count - 1];
      mat_sys->free_count--;
    } else {
      slot = mat_sys->next_free_index;
      while (slot < mat_sys->materials.length &&
             mat_sys->materials.data[slot].id != 0) {
        slot++;
      }
      if (slot >= mat_sys->materials.length) {
        out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
        continue;
      }
      mat_sys->next_free_index = slot + 1;
    }

    size_t name_len = string_length(parsed_data[i].name);
    VkrAllocator mat_alloc = {.ctx = mat_sys->arena};
    vkr_allocator_arena(&mat_alloc);
    char *stable_name = vkr_allocator_alloc(&mat_alloc, name_len + 1,
                                            VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (!stable_name) {
      out_errors[i] = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      continue;
    }
    MemCopy(stable_name, parsed_data[i].name, name_len);
    stable_name[name_len] = '\0';

    VkrMaterial *material = &mat_sys->materials.data[slot];
    MemZero(material, sizeof(VkrMaterial));
    material->id = slot + 1;
    material->generation = mat_sys->generation_counter++;
    material->name = stable_name;
    material->pipeline_id = parsed_data[i].pipeline_id;
    material->phong = parsed_data[i].phong;

    if (parsed_data[i].shader_name[0] != '\0') {
      size_t shader_len = string_length(parsed_data[i].shader_name);
      char *stable_shader = vkr_allocator_alloc(
          &mat_alloc, shader_len + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
      if (stable_shader) {
        MemCopy(stable_shader, parsed_data[i].shader_name, shader_len);
        stable_shader[shader_len] = '\0';
        material->shader_name = stable_shader;
      }
    }

    for (uint32_t tex_slot = 0; tex_slot < VKR_TEXTURE_SLOT_COUNT; tex_slot++) {
      material->textures[tex_slot].slot = (VkrTextureSlot)tex_slot;
      material->textures[tex_slot].handle = VKR_TEXTURE_HANDLE_INVALID;
      material->textures[tex_slot].enabled = false;
    }

    material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
        vkr_texture_system_get_default_handle(mat_sys->texture_system);
    material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true;
    material->textures[VKR_TEXTURE_SLOT_NORMAL].handle =
        vkr_texture_system_get_default_normal_handle(mat_sys->texture_system);
    material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled = true;
    material->textures[VKR_TEXTURE_SLOT_SPECULAR].handle =
        vkr_texture_system_get_default_specular_handle(mat_sys->texture_system);
    material->textures[VKR_TEXTURE_SLOT_SPECULAR].enabled = true;

    // Find and assign batch-loaded textures for this material
    for (uint32_t t = 0; t < total_textures; t++) {
      if (texture_material_index[t] == i && texture_handles[t].id != 0) {
        VkrTextureSlot slot_type = (VkrTextureSlot)texture_slot[t];
        VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
        VkrTextureHandle acquired = vkr_texture_system_acquire(
            mat_sys->texture_system, texture_paths[t], true_v, &acquire_err);
        if (acquired.id != 0) {
          material->textures[slot_type].handle = acquired;
          material->textures[slot_type].enabled = true;
        }
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
        // Note: we don't increment loaded here since these are duplicates
      }
    }
  }

  vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  log_debug("Material batch completed: %u/%u materials loaded (from %u unique)",
            loaded, count, unique_count);
  return loaded;
}
