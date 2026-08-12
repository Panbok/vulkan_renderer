#include "renderer/systems/vkr_material_system.h"

#include "defines.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_resource_system.h"

#define VKR_MATERIAL_SYSTEM_ASYNC_DMEMORY_INITIAL MB(1)
#define VKR_MATERIAL_SYSTEM_ASYNC_DMEMORY_RESERVE MB(16)

typedef struct VkrGizmoMaterialDef {
  const char *name;
  Vec4 emission;
} VkrGizmoMaterialDef;

VkrMaterialAlphaMode
vkr_material_system_material_alpha_mode(const VkrMaterialSystem *system,
                                        const VkrMaterial *material) {
  if (!system || !system->texture_system || !material) {
    return VKR_MATERIAL_ALPHA_OPAQUE;
  }

  if (material->alpha_mode_explicit) {
    if (material->alpha_mode == VKR_MATERIAL_ALPHA_CUTOUT &&
        material->alpha_cutoff <= 0.0f) {
      return VKR_MATERIAL_ALPHA_OPAQUE;
    }
    return material->alpha_mode;
  }

  float32_t factor_alpha = material->material_type == VKR_MATERIAL_TYPE_PBR
                               ? material->pbr.base_color.w
                               : material->phong.diffuse_color.w;
  if (factor_alpha < 0.999f) {
    return VKR_MATERIAL_ALPHA_BLEND;
  }

  const VkrMaterialTexture *diffuse_tex =
      &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
  if (!diffuse_tex->enabled || diffuse_tex->handle.id == 0) {
    return VKR_MATERIAL_ALPHA_OPAQUE;
  }

  VkrTexture *texture = vkr_texture_system_get_by_handle(system->texture_system,
                                                         diffuse_tex->handle);
  if (!texture) {
    return VKR_MATERIAL_ALPHA_OPAQUE;
  }

  if (!bitset8_is_set(&texture->description.properties,
                      VKR_TEXTURE_PROPERTY_HAS_TRANSPARENCY_BIT)) {
    return VKR_MATERIAL_ALPHA_OPAQUE;
  }

  if (material->alpha_cutoff <= 0.0f) {
    return VKR_MATERIAL_ALPHA_OPAQUE;
  }

  if (bitset8_is_set(&texture->description.properties,
                     VKR_TEXTURE_PROPERTY_ALPHA_MASK_BIT)) {
    return VKR_MATERIAL_ALPHA_CUTOUT;
  }

  return VKR_MATERIAL_ALPHA_BLEND;
}

bool8_t
vkr_material_system_material_has_transparency(const VkrMaterialSystem *system,
                                              const VkrMaterial *material) {
  return vkr_material_system_material_alpha_mode(system, material) ==
                 VKR_MATERIAL_ALPHA_BLEND
             ? true_v
             : false_v;
}

bool8_t
vkr_material_system_material_uses_cutout(const VkrMaterialSystem *system,
                                         const VkrMaterial *material) {
  return vkr_material_system_material_alpha_mode(system, material) ==
                 VKR_MATERIAL_ALPHA_CUTOUT
             ? true_v
             : false_v;
}

bool8_t
vkr_material_system_material_is_transmissive(const VkrMaterialSystem *system,
                                             const VkrMaterial *material) {
  if (!material || material->material_type != VKR_MATERIAL_TYPE_PBR) {
    return false_v;
  }
  if (material->pbr.transmission_factor > 0.0f) {
    return true_v;
  }
  const VkrMaterialTexture *texture =
      &material->textures[VKR_TEXTURE_SLOT_TRANSMISSION];
  if (!texture->enabled || texture->handle.id == 0) {
    return false_v;
  }
  if (!system || !system->texture_system) {
    return true_v;
  }
  VkrTexture *resolved =
      vkr_texture_system_get_by_handle(system->texture_system, texture->handle);
  return resolved && resolved->handle ? true_v : false_v;
}

float32_t
vkr_material_system_material_alpha_cutoff(const VkrMaterialSystem *system,
                                          const VkrMaterial *material) {
  if (!system || !material) {
    return 0.0f;
  }

  if (vkr_material_system_material_alpha_mode(system, material) !=
      VKR_MATERIAL_ALPHA_CUTOUT) {
    return 0.0f;
  }

  if (material->alpha_cutoff > 0.0f) {
    return material->alpha_cutoff;
  }

  return VKR_MATERIAL_ALPHA_CUTOFF_DEFAULT;
}

vkr_internal bool8_t
vkr_material_system_find_by_name(VkrMaterialSystem *system, const char *name,
                                 VkrMaterialHandle *out_handle) {
  VkrMaterialEntry *entry =
      vkr_hash_table_get_VkrMaterialEntry(&system->material_by_name, name);
  if (!entry) {
    return false_v;
  }

  VkrMaterial *material = &system->materials.data[entry->id];
  if (!material || material->id == 0) {
    return false_v;
  }

  if (out_handle) {
    *out_handle = (VkrMaterialHandle){.id = material->id,
                                      .generation = material->generation};
  }
  return true_v;
}

vkr_internal void
vkr_material_system_reset_texture_slots(VkrMaterial *material) {
  if (!material) {
    return;
  }

  for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; i++) {
    material->textures[i].slot = (VkrTextureSlot)i;
    material->textures[i].handle = VKR_TEXTURE_HANDLE_INVALID;
    material->textures[i].enabled = false;
  }
}

vkr_internal void
vkr_material_system_apply_default_surface_textures(VkrMaterialSystem *system,
                                                   VkrMaterial *material) {
  if (!system || !material) {
    return;
  }

  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      vkr_texture_system_get_default_diffuse_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true;
  material->textures[VKR_TEXTURE_SLOT_NORMAL].handle =
      vkr_texture_system_get_default_normal_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled = true;
  material->textures[VKR_TEXTURE_SLOT_SPECULAR].handle =
      vkr_texture_system_get_default_specular_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_SPECULAR].enabled = true;
}

vkr_internal void vkr_material_system_init_surface_material(
    VkrMaterialSystem *system, VkrMaterial *material, Vec4 diffuse_color,
    float32_t shininess) {
  if (!material) {
    return;
  }

  MemZero(material, sizeof(*material));
  material->material_type = VKR_MATERIAL_TYPE_PHONG;
  material->alpha_mode = VKR_MATERIAL_ALPHA_OPAQUE;
  material->alpha_mode_explicit = false_v;
  material->double_sided = false_v;
  material->phong.diffuse_color = diffuse_color;
  material->phong.specular_color = vec4_new(1, 1, 1, 1);
  material->phong.emission_color = vec3_zero();
  material->phong.shininess = shininess;
  material->pbr.base_color = diffuse_color;
  material->pbr.metallic = 1.0f;
  material->pbr.roughness = 1.0f;
  material->pbr.normal_scale = 1.0f;
  material->pbr.occlusion_strength = 1.0f;
  material->pbr.emissive_factor = vec3_zero();
  material->pbr.dielectric_specular = vec3_new(0.04f, 0.04f, 0.04f);
  material->pbr.transmission_factor = 0.0f;
  material->pbr.ior = 1.5f;
  material->pbr.thickness_factor = 0.0f;
  material->pbr.attenuation_color = vec3_new(1.0f, 1.0f, 1.0f);
  material->pbr.attenuation_distance = 0.0f;
  material->pipeline_id = VKR_INVALID_ID;

  vkr_material_system_reset_texture_slots(material);
  vkr_material_system_apply_default_surface_textures(system, material);
}

bool8_t vkr_material_system_publish(VkrMaterialSystem *system,
                                    VkrMaterialHandle handle,
                                    VkrRendererError *out_error) {
  assert_log(system != NULL, "Material system is NULL");
  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_NONE;
  }
  if (!system->asset_publisher || !system->asset_publisher->publish_material) {
    return true_v;
  }
  if (handle.id == 0 || handle.id > system->materials.length) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_INVALID_HANDLE;
    }
    return false_v;
  }

  VkrMaterial *material = &system->materials.data[handle.id - 1];
  VkrMaterial published = *material;
  published.alpha_mode =
      vkr_material_system_material_alpha_mode(system, material);
  published.alpha_mode_explicit = true_v;
  if (material->id != handle.id || material->generation != handle.generation ||
      !system->asset_publisher->publish_material(system->asset_publisher->state,
                                                 handle, &published)) {
    if (out_error) {
      *out_error = VKR_RENDERER_ERROR_RESOURCE_CREATION_FAILED;
    }
    return false_v;
  }
  return true_v;
}

bool8_t vkr_material_system_unpublish(VkrMaterialSystem *system,
                                      VkrMaterialHandle handle) {
  assert_log(system != NULL, "Material system is NULL");
  if (!system->asset_publisher ||
      !system->asset_publisher->unpublish_material) {
    return true_v;
  }
  if (!system->asset_publisher->unpublish_material(
          system->asset_publisher->state, handle)) {
    log_warn("MaterialSystem: failed to unpublish material %u:%u", handle.id,
             handle.generation);
    return false_v;
  }
  return true_v;
}

bool8_t vkr_material_system_init(VkrMaterialSystem *system, Arena *arena,
                                 VkrTextureSystem *texture_system,
                                 const VkrMaterialSystemConfig *config) {
  assert_log(system != NULL, "Material system is NULL");
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(texture_system != NULL, "Texture system is NULL");
  assert_log(config != NULL, "Config is NULL");
  assert_log(config->asset_publisher != NULL, "Asset publisher is NULL");

  MemZero(system, sizeof(*system));

  ArenaFlags app_arena_flags = bitset8_create();
  bitset8_set(&app_arena_flags, ARENA_FLAG_LARGE_PAGES);
  system->arena =
      arena_create(VKR_MATERIAL_SYSTEM_DEFAULT_ARENA_RSV,
                   VKR_MATERIAL_SYSTEM_DEFAULT_ARENA_CMT, app_arena_flags);
  system->allocator = (VkrAllocator){.ctx = system->arena};
  vkr_allocator_arena(&system->allocator);

  system->string_allocator.ctx = &system->string_memory;
  if (!vkr_dmemory_create(MB(1), MB(8), &system->string_memory)) {
    log_error("Failed to create material system string allocator");
    arena_destroy(system->arena);
    MemZero(system, sizeof(*system));
    return false_v;
  }
  vkr_dmemory_allocator_create(&system->string_allocator);
  if (!vkr_dmemory_create(VKR_MATERIAL_SYSTEM_ASYNC_DMEMORY_INITIAL,
                          VKR_MATERIAL_SYSTEM_ASYNC_DMEMORY_RESERVE,
                          &system->async_memory)) {
    log_error("Failed to create material system async allocator");
    vkr_dmemory_allocator_destroy(&system->string_allocator);
    arena_destroy(system->arena);
    MemZero(system, sizeof(*system));
    return false_v;
  }
  system->async_allocator = (VkrAllocator){.ctx = &system->async_memory};
  vkr_dmemory_allocator_create(&system->async_allocator);
  if (!vkr_mutex_create(&system->allocator, &system->async_mutex)) {
    log_error("Failed to create material system async allocator mutex");
    vkr_dmemory_allocator_destroy(&system->async_allocator);
    vkr_dmemory_allocator_destroy(&system->string_allocator);
    arena_destroy(system->arena);
    MemZero(system, sizeof(*system));
    return false_v;
  }

  system->texture_system = texture_system;
  system->asset_publisher = config->asset_publisher;
  system->ibl_intensity = 1.0f;
  system->ibl_diffuse_intensity = 1.0f;
  system->ibl_specular_intensity = 1.0f;
  for (uint32_t i = 0; i < 3u; ++i) {
    system->ibl_probe_slots[i] = (VkrMaterialIblProbeSlot){
        .irradiance_map = NULL,
        .prefilter_map = NULL,
        .center = {0},
        .extents = {0},
        .blend_distance = 0.0f,
        .weight = (i == 2u) ? 1.0f : 0.0f,
        .intensity = 1.0f,
        .diffuse_intensity = 1.0f,
        .specular_intensity = 1.0f,
        .box_projection_enabled = false_v,
    };
  }
  system->config = *config;
  system->materials =
      array_create_VkrMaterial(&system->allocator, config->max_material_count);

  uint64_t hash_size = ((uint64_t)config->max_material_count) * 2ULL;
  if (hash_size > UINT32_MAX) {
    log_fatal("Hash table size overflow for max_material_count %u",
              config->max_material_count);
    vkr_mutex_destroy(&system->allocator, &system->async_mutex);
    vkr_dmemory_allocator_destroy(&system->async_allocator);
    vkr_dmemory_allocator_destroy(&system->string_allocator);
    arena_destroy(system->arena);
    MemZero(system, sizeof(*system));
    return false_v;
  }
  system->material_by_name =
      vkr_hash_table_create_VkrMaterialEntry(&system->allocator, hash_size);

  system->free_ids =
      array_create_uint32_t(&system->allocator, config->max_material_count);
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
  if (system->default_material.id == 0) {
    vkr_material_system_shutdown(system);
    return false_v;
  }
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
  if (system->asset_publisher) {
    for (uint32_t i = 0; i < system->materials.length; ++i) {
      VkrMaterial *material = &system->materials.data[i];
      if (material->id == 0) {
        continue;
      }
      (void)vkr_material_system_unpublish(
          system, (VkrMaterialHandle){.id = material->id,
                                      .generation = material->generation});
    }
  }
  array_destroy_VkrMaterial(&system->materials);
  array_destroy_uint32_t(&system->free_ids);
  if (system->async_mutex) {
    vkr_mutex_destroy(&system->allocator, &system->async_mutex);
  }
  if (system->async_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&system->async_allocator);
  }
  if (system->string_allocator.ctx) {
    vkr_dmemory_allocator_destroy(&system->string_allocator);
  }
  if (system->arena)
    arena_destroy(system->arena);
  MemZero(system, sizeof(*system));
}

VkrMaterialHandle
vkr_material_system_create_default(VkrMaterialSystem *system) {
  assert_log(system != NULL, "Material system is NULL");

  VkrMaterial *material = &system->materials.data[0];
  vkr_material_system_init_surface_material(system, material,
                                            vec4_new(1, 1, 1, 1), 8.0f);
  material->id = 1; // slot 0 -> id 1
  material->generation = system->generation_counter++;
  material->name = "__default";

  if (system->next_free_index == 0)
    system->next_free_index = 1;

  VkrMaterialHandle handle = {.id = material->id,
                              .generation = material->generation};
  if (!vkr_material_system_publish(system, handle, NULL)) {
    MemZero(material, sizeof(*material));
    system->next_free_index = 0;
    return VKR_MATERIAL_HANDLE_INVALID;
  }
  return handle;
}

VkrMaterialHandle
vkr_material_system_create_colored(VkrMaterialSystem *system, const char *name,
                                   Vec4 diffuse_color,
                                   VkrRendererError *out_error) {
  assert_log(system != NULL, "Material system is NULL");
  assert_log(name != NULL, "Material name is NULL");

  if (out_error)
    *out_error = VKR_RENDERER_ERROR_NONE;

  // Check if material with this name already exists
  VkrMaterialEntry *existing =
      vkr_hash_table_get_VkrMaterialEntry(&system->material_by_name, name);
  if (existing) {
    existing->ref_count++;
    VkrMaterial *m = &system->materials.data[existing->id];
    return (VkrMaterialHandle){.id = m->id, .generation = m->generation};
  }

  // Find a free slot
  uint32_t slot;
  if (system->free_count > 0) {
    slot = system->free_ids.data[--system->free_count];
  } else {
    if (system->next_free_index >= system->materials.length) {
      log_error("Material system capacity exceeded");
      if (out_error)
        *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
      return (VkrMaterialHandle){0};
    }
    slot = system->next_free_index++;
  }

  // Copy name to string memory
  uint64_t name_len = string_length(name);
  char *name_copy = (char *)vkr_allocator_alloc(
      &system->string_allocator, name_len + 1, VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!name_copy) {
    log_error("Failed to allocate material name");
    // Return slot to free pool
    system->free_ids.data[system->free_count++] = slot;
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return (VkrMaterialHandle){0};
  }
  MemCopy(name_copy, name, name_len);
  name_copy[name_len] = '\0';

  // Initialize material
  VkrMaterial *material = &system->materials.data[slot];
  vkr_material_system_init_surface_material(system, material, diffuse_color,
                                            8.0f);
  material->id = slot + 1;
  material->generation = system->generation_counter++;
  material->name = name_copy;

  // Register in hash table
  VkrMaterialEntry entry = {
      .id = slot,
      .ref_count = 1,
      .auto_release = true_v,
      .name = name_copy,
  };
  if (!vkr_hash_table_insert_VkrMaterialEntry(&system->material_by_name,
                                              name_copy, entry)) {
    vkr_allocator_free(&system->string_allocator, name_copy, name_len + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    MemZero(material, sizeof(*material));
    system->free_ids.data[system->free_count++] = slot;
    if (out_error)
      *out_error = VKR_RENDERER_ERROR_OUT_OF_MEMORY;
    return VKR_MATERIAL_HANDLE_INVALID;
  }

  VkrMaterialHandle handle = {.id = material->id,
                              .generation = material->generation};
  if (!vkr_material_system_publish(system, handle, out_error)) {
    vkr_hash_table_remove_VkrMaterialEntry(&system->material_by_name,
                                           name_copy);
    vkr_allocator_free(&system->string_allocator, name_copy, name_len + 1,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
    MemZero(material, sizeof(*material));
    system->free_ids.data[system->free_count++] = slot;
    return VKR_MATERIAL_HANDLE_INVALID;
  }

  return handle;
}

bool8_t
vkr_material_system_create_gizmo_materials(VkrMaterialSystem *system,
                                           VkrMaterialHandle out_handles[3],
                                           VkrRendererError *out_error) {
  assert_log(system != NULL, "Material system is NULL");

  if (out_error) {
    *out_error = VKR_RENDERER_ERROR_NONE;
  }

  const VkrGizmoMaterialDef defs[] = {
      {.name = "gizmo_axis_x", .emission = vec4_new(1.0f, 0.0f, 0.0f, 1.0f)},
      {.name = "gizmo_axis_y", .emission = vec4_new(0.0f, 1.0f, 0.0f, 1.0f)},
      {.name = "gizmo_axis_z", .emission = vec4_new(0.0f, 0.0f, 1.0f, 1.0f)},
  };

  for (uint32_t i = 0; i < ArrayCount(defs); ++i) {
    VkrMaterialHandle handle = VKR_MATERIAL_HANDLE_INVALID;
    if (!vkr_material_system_find_by_name(system, defs[i].name, &handle)) {
      VkrRendererError err = VKR_RENDERER_ERROR_NONE;
      handle = vkr_material_system_create_colored(
          system, defs[i].name, vec4_new(0.0f, 0.0f, 0.0f, 1.0f), &err);
      if (handle.id == 0) {
        if (out_error) {
          *out_error = err;
        }
        return false_v;
      }
    }

    VkrMaterial *material = vkr_material_system_get_by_handle(system, handle);
    if (!material) {
      if (out_error) {
        *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
      }
      return false_v;
    }

    material->phong.diffuse_color = vec4_new(0.0f, 0.0f, 0.0f, 1.0f);
    material->phong.specular_color = vec4_new(0.5f, 0.5f, 0.5f, 1.0f);
    material->phong.emission_color = defs[i].emission;
    material->phong.shininess = 8.0f;
    material->shader_name = "shader.default.world";

    if (system->asset_publisher) {
      if (!vkr_material_system_unpublish(system, handle) ||
          !vkr_material_system_publish(system, handle, out_error)) {
        return false_v;
      }
    }

    VkrMaterialEntry *entry = vkr_hash_table_get_VkrMaterialEntry(
        &system->material_by_name, defs[i].name);
    if (entry) {
      entry->auto_release = false_v;
      if (entry->ref_count == 0) {
        entry->ref_count = 1;
      }
    }

    if (out_handles) {
      out_handles[i] = handle;
    }
  }

  return true_v;
}

VkrMaterialHandle vkr_material_system_acquire(VkrMaterialSystem *system,
                                              String8 name,
                                              bool8_t auto_release,
                                              VkrRendererError *out_error) {
  assert_log(system != NULL, "Material system is NULL");
  assert_log(out_error != NULL, "Out error is NULL");

  if (!name.str) {
    log_warn("Attempted to acquire material with NULL name, using default");
    *out_error = VKR_RENDERER_ERROR_INVALID_PARAMETER;
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
    *out_error = VKR_RENDERER_ERROR_NONE;
    VkrMaterial *m = &system->materials.data[entry->id];
    return (VkrMaterialHandle){.id = m->id, .generation = m->generation};
  }

  *out_error = VKR_RENDERER_ERROR_RESOURCE_NOT_LOADED;
  return (VkrMaterialHandle){.id = 0, .generation = 0};
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
    uint64_t name_length = string_length(material->name);
    if (name_length == 0) {
      log_warn("Material '%s' has empty name; skipping unload", material->name);
      entry->auto_release = false_v;
      return;
    }
    String8 name =
        string8_create_from_cstr((const uint8_t *)material->name, name_length);
    VkrResourceHandleInfo handle_info = {
        .type = VKR_RESOURCE_TYPE_MATERIAL,
        .loader_id =
            vkr_resource_system_get_loader_id(VKR_RESOURCE_TYPE_MATERIAL, name),
        .as.material = handle};
    vkr_resource_system_unload(&handle_info, name);
  }
}

void vkr_material_system_add_ref(VkrMaterialSystem *system,
                                 VkrMaterialHandle handle) {
  assert_log(system != NULL, "System is NULL");
  assert_log(handle.id != 0, "Handle is invalid");

  uint32_t idx = handle.id - 1;
  if (idx >= system->materials.length)
    return;

  VkrMaterial *material = &system->materials.data[idx];
  if (material->generation != handle.generation || material->id == 0 ||
      !material->name)
    return;

  VkrMaterialEntry *entry = vkr_hash_table_get_VkrMaterialEntry(
      &system->material_by_name, material->name);
  if (!entry)
    return;

  entry->ref_count++;
}

void vkr_material_system_set_shadow_map(VkrMaterialSystem *system,
                                        VkrTextureOpaqueHandle map,
                                        bool8_t enabled) {
  assert_log(system != NULL, "System is NULL");

  if (!enabled || !map) {
    system->shadow_map = NULL;
    system->shadow_maps_enabled = false_v;
    return;
  }

  system->shadow_map = map;
  system->shadow_maps_enabled = true_v;
}

void vkr_material_system_set_ibl_maps(VkrMaterialSystem *system,
                                      VkrTextureOpaqueHandle irradiance_map,
                                      VkrTextureOpaqueHandle prefilter_map,
                                      bool8_t enabled, float32_t intensity,
                                      float32_t diffuse_intensity,
                                      float32_t specular_intensity) {
  assert_log(system != NULL, "System is NULL");

  system->ibl_irradiance_map = irradiance_map;
  system->ibl_prefilter_map = prefilter_map;
  system->ibl_enabled = enabled ? true_v : false_v;
  system->ibl_intensity = intensity;
  system->ibl_diffuse_intensity = diffuse_intensity;
  system->ibl_specular_intensity = specular_intensity;
}

void vkr_material_system_set_ibl_probe_slots(
    VkrMaterialSystem *system, const VkrMaterialIblProbeSlot slots[3]) {
  assert_log(system != NULL, "System is NULL");

  if (!slots) {
    for (uint32_t i = 0; i < 3u; ++i) {
      system->ibl_probe_slots[i] = (VkrMaterialIblProbeSlot){
          .irradiance_map = NULL,
          .prefilter_map = NULL,
          .center = {0},
          .extents = {0},
          .blend_distance = 0.0f,
          .weight = (i == 2u) ? 1.0f : 0.0f,
          .intensity = 1.0f,
          .diffuse_intensity = 1.0f,
          .specular_intensity = 1.0f,
          .box_projection_enabled = false_v,
      };
    }
    return;
  }

  for (uint32_t i = 0; i < 3u; ++i) {
    system->ibl_probe_slots[i] = slots[i];
    if (system->ibl_probe_slots[i].blend_distance < 0.0f) {
      system->ibl_probe_slots[i].blend_distance = 0.0f;
    }
    if (system->ibl_probe_slots[i].weight < 0.0f) {
      system->ibl_probe_slots[i].weight = 0.0f;
    }
  }
}

void vkr_material_system_set_transmission_source(VkrMaterialSystem *system,
                                                 VkrTextureOpaqueHandle source,
                                                 bool8_t enabled) {
  assert_log(system != NULL, "System is NULL");
  system->transmission_source = enabled ? source : NULL;
  system->transmission_pass_enabled = enabled && source ? true_v : false_v;
}

VkrMaterial *vkr_material_system_get_by_handle(VkrMaterialSystem *system,
                                               VkrMaterialHandle handle) {
  if (!system || handle.id == 0)
    return NULL;
  uint32_t index = handle.id - 1;
  if (index >= system->materials.length)
    return NULL;
  VkrMaterial *material = &system->materials.data[index];
  return (material->generation == handle.generation) ? material : NULL;
}
