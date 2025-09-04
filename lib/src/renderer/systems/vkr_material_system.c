#include "renderer/systems/vkr_material_system.h"

bool8_t vkr_material_system_init(VkrMaterialSystem *system, Arena *arena,
                                 VkrTextureSystem *texture_system,
                                 const VkrMaterialSystemConfig *config) {
  assert_log(system != NULL, "Material system is NULL");
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(texture_system != NULL, "Texture system is NULL");
  assert_log(config != NULL, "Config is NULL");

  MemZero(system, sizeof(*system));

  ArenaFlags app_arena_flags = bitset8_create();
  bitset8_set(&app_arena_flags, ARENA_FLAG_LARGE_PAGES);
  system->arena =
      arena_create(VKR_MATERIAL_SYSTEM_DEFAULT_ARENA_RSV,
                   VKR_MATERIAL_SYSTEM_DEFAULT_ARENA_CMT, app_arena_flags);
  system->temp_arena =
      arena_create(KB(64), KB(64)); // small temp arena for parsing
  if (!system->arena || !system->temp_arena) {
    log_fatal("Failed to create material system arenas");
    if (system->arena)
      arena_destroy(system->arena);
    if (system->temp_arena)
      arena_destroy(system->temp_arena);
    return false_v;
  }

  system->texture_system = texture_system;
  system->config = *config;
  system->materials =
      array_create_VkrMaterial(system->arena, config->max_material_count);
  system->material_by_name = vkr_hash_table_create_VkrMaterialEntry(
      system->arena, ((uint64_t)config->max_material_count) * 2ULL);
  system->free_ids =
      array_create_uint32_t(system->arena, config->max_material_count);
  system->free_count = 0;
  system->next_free_index = 0;
  system->generation_counter = 1;

  // Initialize as empty
  for (uint32_t mat = 0; mat < system->materials.length; mat++) {
    system->materials.data[mat].id = 0;
    system->materials.data[mat].generation = 0;
    system->materials.data[mat].name = NULL;
    system->materials.data[mat].pipeline_id = VKR_INVALID_ID;
  }

  system->default_material = vkr_material_system_create_default(system);
  // Register default in lifetime map with non-releasable entry
  VkrMaterial *def = &system->materials.data[0];
  VkrMaterialEntry def_entry = {
      .id = 0, .ref_count = 1, .auto_release = false_v, .name = def->name};
  vkr_hash_table_insert_VkrMaterialEntry(&system->material_by_name, def->name,
                                         def_entry);
  return true_v;
}

void vkr_material_system_shutdown(VkrMaterialSystem *system) {
  if (!system)
    return;
  array_destroy_VkrMaterial(&system->materials);
  array_destroy_uint32_t(&system->free_ids);
  if (system->arena)
    arena_destroy(system->arena);
  if (system->temp_arena)
    arena_destroy(system->temp_arena);
  MemZero(system, sizeof(*system));
}

VkrMaterialHandle
vkr_material_system_create_default(VkrMaterialSystem *system) {
  assert_log(system != NULL, "Material system is NULL");

  VkrMaterial *m = &system->materials.data[0];
  m->id = 1; // slot 0 -> id 1
  m->generation = system->generation_counter++;
  m->name = "__default";
  m->diffuse_color = vec4_new(1, 1, 1, 1);
  m->specular_color = vec4_new(1, 1, 1, 1);
  m->shininess = 32.0f;
  m->emission_color = vec3_new(0, 0, 0);
  m->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      vkr_texture_system_get_default_handle(system->texture_system);
  m->textures[VKR_TEXTURE_SLOT_DIFFUSE].slot = VKR_TEXTURE_SLOT_DIFFUSE;
  m->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true;

  if (system->next_free_index == 0)
    system->next_free_index = 1;

  VkrMaterialHandle handle = {.id = m->id, .generation = m->generation};
  return handle;
}

VkrMaterialHandle vkr_material_system_acquire(VkrMaterialSystem *system,
                                              String8 name,
                                              bool8_t auto_release) {
  assert_log(system != NULL, "Material system is NULL");

  if (!name.str) {
    log_warn("Attempted to acquire material with NULL name, using default");
    return system->default_material;
  }

  const char *key = (const char *)name.str;
  VkrMaterialEntry *entry =
      vkr_hash_table_get_VkrMaterialEntry(&system->material_by_name, key);
  if (entry) {
    if (entry->ref_count == 0) {
      entry->auto_release = auto_release;
    }
    entry->ref_count++;
    VkrMaterial *m = &system->materials.data[entry->id];
    return (VkrMaterialHandle){.id = m->id, .generation = m->generation};
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
      return system->default_material;
    }
    system->next_free_index = slot + 1;
  }

  VkrMaterial *m = &system->materials.data[slot];
  m->id = slot + 1;
  m->generation = system->generation_counter++;
  // Copy name into system arena to ensure stable lifetime
  char *stable_name =
      arena_alloc(system->arena, name.length + 1, ARENA_MEMORY_TAG_STRING);
  assert_log(stable_name != NULL, "Failed to allocate name for material");
  MemCopy(stable_name, name.str, (size_t)name.length);
  stable_name[name.length] = '\0';
  m->name = (const char *)stable_name;
  m->diffuse_color = vec4_new(1, 1, 1, 1);
  m->specular_color = vec4_new(1, 1, 1, 1);
  m->shininess = 32.0f;
  m->emission_color = vec3_new(0, 0, 0);
  m->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      vkr_texture_system_get_default_handle(system->texture_system);
  m->textures[VKR_TEXTURE_SLOT_DIFFUSE].slot = VKR_TEXTURE_SLOT_DIFFUSE;
  m->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true;

  VkrMaterialEntry new_entry = {.id = slot,
                                .ref_count = 1,
                                .auto_release = auto_release,
                                .name = (const char *)stable_name};
  vkr_hash_table_insert_VkrMaterialEntry(&system->material_by_name,
                                         (const char *)stable_name, new_entry);
  return (VkrMaterialHandle){.id = m->id, .generation = m->generation};
}

void vkr_material_system_set(VkrMaterialSystem *system,
                             VkrMaterialHandle handle,
                             VkrTextureHandle base_color, Vec4 diffuse_color,
                             Vec4 specular_color, float32_t shininess,
                             Vec3 emission_color) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t idx = handle.id - 1;
  if (idx >= system->materials.length)
    return;

  VkrMaterial *m = &system->materials.data[idx];
  if (m->generation != handle.generation)
    return;

  m->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      base_color.id != 0
          ? base_color
          : vkr_texture_system_get_default_handle(system->texture_system);
  m->diffuse_color = diffuse_color;
  m->specular_color = specular_color;
  m->shininess = shininess;
  m->emission_color = emission_color;
  m->generation = system->generation_counter++;
}

void vkr_material_system_release(VkrMaterialSystem *system,
                                 VkrMaterialHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t idx = handle.id - 1;
  if (idx >= system->materials.length)
    return;

  VkrMaterial *m = &system->materials.data[idx];
  if (m->generation != handle.generation || m->id == 0)
    return;

  if (!m->name)
    return;

  VkrMaterialEntry *entry =
      vkr_hash_table_get_VkrMaterialEntry(&system->material_by_name, m->name);
  if (!entry)
    return;

  if (entry->ref_count == 0) {
    log_warn("Over-release detected for material '%s'", m->name);
    return;
  }

  entry->ref_count--;

  bool32_t should_release = (entry->ref_count == 0) && entry->auto_release;
  if (should_release) {
    // Reset material slot and push id to free stack
    m->id = 0;
    m->name = NULL;
    m->pipeline_id = VKR_INVALID_ID;
    m->diffuse_color = vec4_new(1, 1, 1, 1);
    m->specular_color = vec4_new(1, 1, 1, 1);
    m->shininess = 0.0f;
    m->emission_color = vec3_new(0, 0, 0);
    for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; i++) {
      m->textures[i].handle = VKR_TEXTURE_HANDLE_INVALID;
      m->textures[i].enabled = false;
      m->textures[i].slot = (VkrTextureSlot)i;
    }
    assert_log(system->free_count < system->free_ids.length,
               "free_ids overflow in material system");
    system->free_ids.data[system->free_count++] = idx;
    vkr_hash_table_remove_VkrMaterialEntry(&system->material_by_name,
                                           entry->name);
    if (idx < system->next_free_index) {
      system->next_free_index = idx;
    }
  }
}

bool8_t vkr_material_system_load_from_mt(RendererFrontendHandle renderer,
                                         VkrMaterialSystem *system,
                                         String8 path, Arena *temp_arena,
                                         VkrMaterialHandle *out_handle) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(system != NULL, "Material system is NULL");
  assert_log(out_handle != NULL, "Out handle is NULL");

  if (!path.str || path.length == 0) {
    log_error("Attempted to load material from NULL path");
    return false_v;
  }

  FilePath fp = file_path_create((const char *)path.str, temp_arena,
                                 FILE_PATH_TYPE_RELATIVE);
  FileMode mode = bitset8_create();
  bitset8_set(&mode, FILE_MODE_READ);
  FileHandle fh = {0};
  FileError fe = file_open(&fp, mode, &fh);
  if (fe != FILE_ERROR_NONE) {
    log_error("Failed to open material file '%s': %s", (char *)path.str,
              file_get_error_string(fe).str);
    return false_v;
  }

  // Derive material name from basename without extension
  const char *cpath = (const char *)path.str;
  const char *last_slash = strrchr(cpath, '/');
  const char *fname = last_slash ? last_slash + 1 : cpath;
  const char *dot = strrchr(fname, '.');
  char name_buf[128] = {0};
  if (dot && (size_t)(dot - fname) < sizeof(name_buf)) {
    MemCopy(name_buf, fname, (size_t)(dot - fname));
  } else {
    size_t flen = string_length(fname);
    size_t copy_len = flen < sizeof(name_buf) - 1 ? flen : sizeof(name_buf) - 1;
    MemCopy(name_buf, fname, copy_len);
  }

  // Create or get the material handle by name (auto_release by default)
  VkrMaterialHandle handle = vkr_material_system_acquire(
      system,
      string8_create_from_cstr((const uint8_t *)name_buf,
                               string_length(name_buf)),
      true_v);

  // Defaults (Phong)
  VkrTextureHandle base_color =
      vkr_texture_system_get_default_handle(system->texture_system);
  Vec4 diffuse_color = vec4_new(1, 1, 1, 1);
  Vec4 specular_color = vec4_new(1, 1, 1, 1);
  float32_t shininess = 32.0f;
  Vec3 emission_color = vec3_new(0, 0, 0);

  // Read file line by line
  // Use two distinct arenas for file_read_line (system temp for the buffer,
  // caller-provided temp_arena as the line arena)
  Scratch scratch = scratch_create(system->temp_arena);
  while (true) {
    String8 line = {0};
    FileError le = file_read_line(&fh, scratch.arena, temp_arena, 32000, &line);
    if (le == FILE_ERROR_EOF) {
      break;
    }
    if (le != FILE_ERROR_NONE) {
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
    // Strip any stray trailing CR/LF from value to avoid file path issues
    while (value.length > 0 && (value.str[value.length - 1] == '\n' ||
                                value.str[value.length - 1] == '\r')) {
      value.length--;
    }

    if (string8_contains_cstr(&key, "base_color")) {
      // Acquire texture by path
      RendererError tex_err = RENDERER_ERROR_NONE;
      VkrTextureHandle th =
          vkr_texture_system_acquire(renderer, system->texture_system, value,
                                     true_v, temp_arena, &tex_err);
      if (tex_err == RENDERER_ERROR_NONE && th.id != 0) {
        base_color = th;
      } else {
        // Ensure value is NUL-terminated for logging
        char *val_cstr = (char *)arena_alloc(temp_arena, value.length + 1,
                                             ARENA_MEMORY_TAG_STRING);
        MemCopy(val_cstr, value.str, (size_t)value.length);
        val_cstr[value.length] = '\0';
        log_warn("Material '%s': failed to load base_color '%s', using default",
                 name_buf, val_cstr);
      }
    } else if (string8_contains_cstr(&key, "diffuse_color")) {
      Vec4 v;
      if (string8_to_vec4(&value, &v)) {
        diffuse_color = v;
      } else {
        char *val_cstr = (char *)arena_alloc(temp_arena, value.length + 1,
                                             ARENA_MEMORY_TAG_STRING);
        MemCopy(val_cstr, value.str, (size_t)value.length);
        val_cstr[value.length] = '\0';
        log_warn("Material '%s': invalid diffuse_color '%s'", name_buf,
                 val_cstr);
      }
    } else if (string8_contains_cstr(&key, "specular_color")) {
      Vec4 v;
      if (string8_to_vec4(&value, &v)) {
        specular_color = v;
      } else {
        char *val_cstr = (char *)arena_alloc(temp_arena, value.length + 1,
                                             ARENA_MEMORY_TAG_STRING);
        MemCopy(val_cstr, value.str, (size_t)value.length);
        val_cstr[value.length] = '\0';
        log_warn("Material '%s': invalid specular_color '%s'", name_buf,
                 val_cstr);
      }
    } else if (string8_contains_cstr(&key, "shininess")) {
      float32_t s = 0.0f;
      if (string8_to_f32(&value, &s)) {
        shininess = s;
      } else {
        char *val_cstr = (char *)arena_alloc(temp_arena, value.length + 1,
                                             ARENA_MEMORY_TAG_STRING);
        MemCopy(val_cstr, value.str, (size_t)value.length);
        val_cstr[value.length] = '\0';
        log_warn("Material '%s': invalid shininess '%s'", name_buf, val_cstr);
      }
    } else if (string8_contains_cstr(&key, "emission_color")) {
      Vec3 v;
      if (string8_to_vec3(&value, &v)) {
        emission_color = v;
      } else {
        char *val_cstr = (char *)arena_alloc(temp_arena, value.length + 1,
                                             ARENA_MEMORY_TAG_STRING);
        MemCopy(val_cstr, value.str, (size_t)value.length);
        val_cstr[value.length] = '\0';
        log_warn("Material '%s': invalid emission_color '%s'", name_buf,
                 val_cstr);
      }
    } else {
      // pipeline or unknown keys can be ignored for now
      log_warn("Material '%s': unknown key '%s'", name_buf, key.str);
    }
  }
  scratch_destroy(scratch, ARENA_MEMORY_TAG_STRING);
  file_close(&fh);

  vkr_material_system_set(system, handle, base_color, diffuse_color,
                          specular_color, shininess, emission_color);
  // Sync handle generation after set() may bump generation
  if (handle.id != 0) {
    uint32_t idx_handle = handle.id - 1;
    if (idx_handle < system->materials.length) {
      handle.generation = system->materials.data[idx_handle].generation;
    }
  }
  *out_handle = handle;
  return true_v;
}

bool8_t
vkr_material_system_load_default_from_mt(RendererFrontendHandle renderer,
                                         VkrMaterialSystem *system,
                                         String8 path, Arena *temp_arena) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(system != NULL, "System is NULL");
  assert_log(path.str != NULL, "Path is NULL");
  assert_log(temp_arena != NULL, "Temp arena is NULL");

  VkrMaterialHandle h = {0};
  bool8_t ok =
      vkr_material_system_load_from_mt(renderer, system, path, temp_arena, &h);
  if (!ok)
    return false_v;
  // Keep default id but copy properties from the loaded handle
  // Get loaded material by handle
  VkrMaterial *loaded = NULL;
  if (h.id != 0) {
    uint32_t idx = h.id - 1;
    if (idx < system->materials.length) {
      VkrMaterial *candidate = &system->materials.data[idx];
      if (candidate->generation == h.generation) {
        loaded = candidate;
      }
    }
  }
  if (!loaded) {
    return false_v;
  }
  VkrMaterial *def = &system->materials.data[0];
  def->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      loaded->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle;
  def->diffuse_color = loaded->diffuse_color;
  def->specular_color = loaded->specular_color;
  def->shininess = loaded->shininess;
  def->emission_color = loaded->emission_color;
  def->generation = system->generation_counter++;
  // Keep the stored default handle in sync with the new generation
  system->default_material.generation = def->generation;

  // Release the loaded material entry (auto_release should clean it up)
  vkr_material_system_release(system, h);
  return true_v;
}
