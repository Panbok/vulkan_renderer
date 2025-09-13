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

  uint64_t hash_size = ((uint64_t)config->max_material_count) * 2ULL;
  if (hash_size > UINT32_MAX) {
    log_fatal("Hash table size overflow for max_material_count %u",
              config->max_material_count);
    arena_destroy(system->arena);
    arena_destroy(system->temp_arena);
    return false_v;
  }
  system->material_by_name =
      vkr_hash_table_create_VkrMaterialEntry(system->arena, hash_size);

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

  VkrMaterial *material = &system->materials.data[0];
  material->id = 1; // slot 0 -> id 1
  material->generation = system->generation_counter++;
  material->name = "__default";
  material->phong.diffuse_color = vec4_new(1, 1, 1, 1);
  material->phong.specular_color = vec4_new(1, 1, 1, 1);
  material->phong.shininess = 32.0f;
  material->phong.emission_color = vec3_new(0, 0, 0);
  // Initialize all texture slots disabled
  for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; i++) {
    material->textures[i].slot = (VkrTextureSlot)i;
    material->textures[i].handle = VKR_TEXTURE_HANDLE_INVALID;
    material->textures[i].enabled = false;
  }
  // Set default diffuse
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      vkr_texture_system_get_default_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true;

  if (system->next_free_index == 0)
    system->next_free_index = 1;

  VkrMaterialHandle handle = {.id = material->id,
                              .generation = material->generation};
  return handle;
}

VkrMaterialHandle vkr_material_system_acquire(VkrMaterialSystem *system,
                                              String8 name,
                                              bool8_t auto_release,
                                              RendererError *out_error) {
  assert_log(system != NULL, "Material system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!name.str) {
    log_warn("Attempted to acquire material with NULL name, using default");
    *out_error = RENDERER_ERROR_INVALID_PARAMETER;
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
    *out_error = RENDERER_ERROR_NONE;
    VkrMaterial *m = &system->materials.data[entry->id];
    return (VkrMaterialHandle){.id = m->id, .generation = m->generation};
  }

  // Material not loaded - return error
  log_warn("Material '%s' not yet loaded, use resource system to load first",
           string8_cstr(&name));
  *out_error = RENDERER_ERROR_RESOURCE_NOT_LOADED;
  return (VkrMaterialHandle){.id = 0, .generation = 0};
}

void vkr_material_system_set(VkrMaterialSystem *system,
                             VkrMaterialHandle handle,
                             VkrTextureHandle base_color,
                             VkrPhongProperties phong) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t idx = handle.id - 1;
  if (idx >= system->materials.length)
    return;

  VkrMaterial *material = &system->materials.data[idx];
  if (material->generation != handle.generation)
    return;

  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      base_color.id != 0
          ? base_color
          : vkr_texture_system_get_default_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].slot = VKR_TEXTURE_SLOT_DIFFUSE;
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = (base_color.id != 0);

  material->phong = phong;
  material->generation = system->generation_counter++;
}

void vkr_material_system_set_texture(VkrMaterialSystem *system,
                                     VkrMaterialHandle handle,
                                     VkrTextureSlot slot,
                                     VkrTextureHandle texture_handle,
                                     bool8_t enable) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t index = handle.id - 1;
  if (index >= system->materials.length)
    return;

  VkrMaterial *material = &system->materials.data[index];
  if (material->generation != handle.generation)
    return;

  material->textures[slot].slot = slot;
  if (texture_handle.id == 0) {
    material->textures[slot].handle = VKR_TEXTURE_HANDLE_INVALID;
    material->textures[slot].enabled = false;
  } else {
    material->textures[slot].handle = texture_handle;
    material->textures[slot].enabled = enable;
  }
  material->generation = system->generation_counter++;
}

void vkr_material_system_set_textures(
    VkrMaterialSystem *system, VkrMaterialHandle handle,
    const VkrTextureHandle textures[VKR_TEXTURE_SLOT_COUNT],
    const bool8_t enabled[VKR_TEXTURE_SLOT_COUNT]) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t index = handle.id - 1;
  if (index >= system->materials.length)
    return;

  VkrMaterial *material = &system->materials.data[index];
  if (material->generation != handle.generation)
    return;

  for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; i++) {
    material->textures[i].slot = (VkrTextureSlot)i;
    if (textures[i].id == 0) {
      material->textures[i].handle = VKR_TEXTURE_HANDLE_INVALID;
      material->textures[i].enabled = false;
    } else {
      material->textures[i].handle = textures[i];
      material->textures[i].enabled = enabled ? enabled[i] : true_v;
    }
  }
  material->generation = system->generation_counter++;
}

void vkr_material_system_release(VkrMaterialSystem *system,
                                 VkrMaterialHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t idx = handle.id - 1;
  if (idx >= system->materials.length)
    return;

  VkrMaterial *material = &system->materials.data[idx];
  if (material->generation != handle.generation || material->id == 0)
    return;

  if (!material->name)
    return;

  VkrMaterialEntry *entry = vkr_hash_table_get_VkrMaterialEntry(
      &system->material_by_name, material->name);
  if (!entry)
    return;

  if (entry->ref_count == 0) {
    log_warn("Over-release detected for material '%s'", material->name);
    return;
  }

  entry->ref_count--;

  if (entry->ref_count == 0 && entry->auto_release) {
    String8 name = string8_create_from_cstr((const uint8_t *)material->name,
                                            string_length(material->name));
    VkrResourceHandleInfo handle_info = {
        .type = VKR_RESOURCE_TYPE_MATERIAL,
        .loader_id =
            vkr_resource_system_get_loader_id(VKR_RESOURCE_TYPE_MATERIAL, name),
        .as.material = handle};
    vkr_resource_system_unload(&handle_info, name);
  }
}
