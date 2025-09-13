#include "renderer/resources/loaders/material_loader.h"

vkr_global const char *mt_ext = "mt";

/**
 * @brief Arena for storing file buffer. Loaders are never unloaded, so we can
 * reuse the same arena for all loaders.
 */
vkr_global Arena *file_buffer_arena = NULL;

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

// Forward declarations
vkr_internal RendererError
vkr_material_loader_load_from_mt(VkrResourceLoader *self, String8 path,
                                 Arena *temp_arena, VkrMaterial *out_material);

vkr_internal bool8_t vkr_material_loader_can_load(VkrResourceLoader *self,
                                                  String8 name) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");

  const uint8_t *s = name.str;
  for (uint64_t ch = name.length; ch > 0; ch--) {
    if (s[ch - 1] == '.') {
      String8 ext = string8_substring(&name, ch, name.length);
      String8 mt = string8_create_from_cstr((const uint8_t *)mt_ext,
                                            string_length(mt_ext));
      return string8_equalsi(&ext, &mt);
    }
  }

  return false_v;
}

vkr_internal bool8_t vkr_material_loader_load(VkrResourceLoader *self,
                                              String8 name, Arena *temp_arena,
                                              VkrResourceHandleInfo *out_handle,
                                              RendererError *out_error) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(name.str != NULL, "Name is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  VkrMaterialSystem *system = (VkrMaterialSystem *)self->resource_system;

  // Load material from .mt file
  VkrMaterial loaded_material = {0};
  *out_error = vkr_material_loader_load_from_mt(self, name, temp_arena,
                                                &loaded_material);
  if (*out_error != RENDERER_ERROR_NONE) {
    return false_v;
  }

  char material_name_buf[128] = {0};
  String8 material_name = {0};
  vkr_get_stable_material_name(material_name_buf, 128, name, &material_name);

  // Check if material already exists
  const char *material_key = (const char *)material_name.str;
  VkrMaterialEntry *existing_entry = vkr_hash_table_get_VkrMaterialEntry(
      &system->material_by_name, material_key);
  if (existing_entry) {
    log_warn("Material '%s' already exists in system", material_key);
    *out_error = RENDERER_ERROR_RESOURCE_CREATION_FAILED;
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
      *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
      return false_v;
    }
    system->next_free_index = slot + 1;
  }

  // Store stable copy of key in system arena
  char *stable_name = arena_alloc(system->arena, material_name.length + 1,
                                  ARENA_MEMORY_TAG_STRING);
  if (!stable_name) {
    log_error("Failed to allocate name for material");
    *out_error = RENDERER_ERROR_OUT_OF_MEMORY;
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
  *out_error = RENDERER_ERROR_NONE;

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

vkr_internal RendererError
vkr_material_loader_load_from_mt(VkrResourceLoader *self, String8 path,
                                 Arena *temp_arena, VkrMaterial *out_material) {
  assert_log(self != NULL, "Self is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");
  assert_log(out_material != NULL, "Out material is NULL");

  VkrMaterialSystem *material_system =
      (VkrMaterialSystem *)self->resource_system;

  FilePath fp = file_path_create((const char *)path.str, temp_arena,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle fh = {0};
  FileError fe = file_open(&fp, mode, &fh);
  if (fe != FILE_ERROR_NONE) {
    log_error("Failed to open material file '%s': %s", (char *)path.str,
              file_get_error_string(fe).str);
    return RENDERER_ERROR_FILE_NOT_FOUND;
  }

  char material_name_buf[128] = {0};
  String8 name = {0};
  vkr_get_stable_material_name(material_name_buf, 128, path, &name);

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

  if (!file_buffer_arena) {
    file_close(&fh);
    return RENDERER_ERROR_OUT_OF_MEMORY;
  }

  while (true) {
    String8 line = {0};
    FileError le =
        file_read_line(&fh, file_buffer_arena, temp_arena, 32000, &line);
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

    if (string8_contains_cstr(&key, "base_color")) {
      Scratch scratch = scratch_create(temp_arena);
      char *texture_path_cstr = (char *)arena_alloc(
          scratch.arena, value.length + 1, ARENA_MEMORY_TAG_STRING);
      if (!texture_path_cstr) {
        log_error("Failed to allocate texture path buffer");
        continue;
      }
      MemCopy(texture_path_cstr, value.str, (size_t)value.length);
      texture_path_cstr[value.length] = '\0';
      String8 texture_path = string8_create_from_cstr(
          (const uint8_t *)texture_path_cstr, value.length);

      log_debug("Material '%s': base_color texture requested: %s",
                string8_cstr(&name), texture_path_cstr);
      VkrTextureHandle handle = VKR_TEXTURE_HANDLE_INVALID;
      RendererError renderer_error = RENDERER_ERROR_NONE;
      if (!vkr_texture_system_load(material_system->texture_system,
                                   texture_path, &handle, &renderer_error) ||
          renderer_error != RENDERER_ERROR_NONE) {
        log_error("Failed to load texture '%s': %s", texture_path_cstr,
                  renderer_get_error_string(renderer_error).str);
      }

      handle =
          vkr_texture_system_acquire(material_system->texture_system,
                                     texture_path, true_v, &renderer_error);
      if (renderer_error != RENDERER_ERROR_NONE) {
        log_error("Failed to acquire texture '%s': %s", texture_path_cstr,
                  renderer_get_error_string(renderer_error).str);
      }

      scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);

      out_material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle = handle;
    } else if (string8_contains_cstr(&key, "diffuse_color")) {
      Vec4 v;
      if (string8_to_vec4(&value, &v)) {
        out_material->phong.diffuse_color = v;
      } else {
        log_warn("Material '%s': invalid diffuse_color '%s'",
                 string8_cstr(&name), string8_cstr(&value));
      }

    } else if (string8_contains_cstr(&key, "specular_color")) {
      Vec4 v;
      if (string8_to_vec4(&value, &v)) {
        out_material->phong.specular_color = v;
      } else {
        log_warn("Material '%s': invalid specular_color '%s'",
                 string8_cstr(&name), string8_cstr(&value));
      }

    } else if (string8_contains_cstr(&key, "shininess")) {
      float32_t s = 0.0f;

      if (string8_to_f32(&value, &s)) {
        out_material->phong.shininess = s;
      } else {
        log_warn("Material '%s': invalid shininess '%s'", string8_cstr(&name),
                 string8_cstr(&value));
      }

    } else if (string8_contains_cstr(&key, "emission_color")) {
      Vec3 v;
      if (string8_to_vec3(&value, &v)) {
        out_material->phong.emission_color = v;
      } else {
        log_warn("Material '%s': invalid emission_color '%s'",
                 string8_cstr(&name), string8_cstr(&value));
      }

    } else {
      // Unknown keys are ignored for now
      log_debug("Material '%s': ignoring unknown key '%.*s'",
                string8_cstr(&name), (int)key.length, (char *)key.str);
    }
  }

  file_close(&fh);

  return RENDERER_ERROR_NONE;
}

VkrResourceLoader vkr_material_loader_create(void) {
  VkrResourceLoader loader = {0};
  loader.type = VKR_RESOURCE_TYPE_MATERIAL;
  loader.can_load = vkr_material_loader_can_load;
  loader.load = vkr_material_loader_load;
  loader.unload = vkr_material_loader_unload;

  file_buffer_arena = arena_create(KB(64), KB(64));
  if (!file_buffer_arena) {
    log_fatal("Failed to create file buffer arena for material loader");
  }

  return loader;
}