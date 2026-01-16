#include "renderer/systems/vkr_material_system.h"

#include "defines.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_resource_system.h"

typedef struct VkrGizmoMaterialDef {
  const char *name;
  Vec4 emission;
} VkrGizmoMaterialDef;

vkr_internal VkrTextureOpaqueHandle
vkr_material_system_get_shadow_fallback(VkrMaterialSystem *system) {
  if (!system || !system->texture_system) {
    return NULL;
  }
  VkrTexture *fallback = vkr_texture_system_get_default(system->texture_system);
  return fallback ? fallback->handle : NULL;
}

vkr_internal void
vkr_material_system_apply_shadow_samplers(VkrMaterialSystem *system) {
  vkr_local_persist const char
      *k_shadow_samplers[VKR_SHADOW_CASCADE_COUNT_MAX] = {
          "shadow_map_0",
          "shadow_map_1",
          "shadow_map_2",
          "shadow_map_3",
      };

  VkrTextureOpaqueHandle fallback =
      vkr_material_system_get_shadow_fallback(system);

  for (uint32_t i = 0; i < VKR_SHADOW_CASCADE_COUNT_MAX; ++i) {
    VkrTextureOpaqueHandle map = fallback;
    if (system->shadow_maps_enabled && i < system->shadow_map_count &&
        system->shadow_maps[i]) {
      map = system->shadow_maps[i];
    }
    vkr_shader_system_sampler_set(system->shader_system, k_shadow_samplers[i],
                                  map);
  }
}

/**
 * @brief Resolves a material texture handle to a valid 2D GPU texture.
 *
 * Uses the provided fallback when the requested handle is missing, points to a
 * non-2D texture, or does not currently have a backend handle (e.g. during
 * scene reload/async load windows). This prevents writing invalid descriptor
 * bindings (NULL image views/samplers).
 */
vkr_internal VkrTexture *
vkr_material_system_resolve_2d_texture(VkrMaterialSystem *system,
                                       VkrTextureHandle handle,
                                       VkrTextureHandle fallback_handle) {
  if (!system || !system->texture_system) {
    return NULL;
  }

  VkrTexture *texture =
      vkr_texture_system_get_by_handle(system->texture_system, handle);
  if (!texture || texture->description.type != VKR_TEXTURE_TYPE_2D ||
      !texture->handle) {
    texture = vkr_texture_system_get_by_handle(system->texture_system,
                                               fallback_handle);
  }

  return (texture && texture->handle) ? texture : NULL;
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

bool8_t vkr_material_system_init(VkrMaterialSystem *system, Arena *arena,
                                 VkrTextureSystem *texture_system,
                                 VkrShaderSystem *shader_system,
                                 const VkrMaterialSystemConfig *config) {
  assert_log(system != NULL, "Material system is NULL");
  assert_log(arena != NULL, "Arena is NULL");
  assert_log(texture_system != NULL, "Texture system is NULL");
  assert_log(shader_system != NULL, "Shader system is NULL");
  assert_log(config != NULL, "Config is NULL");

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

  system->texture_system = texture_system;
  system->shader_system = shader_system;
  system->config = *config;
  system->materials =
      array_create_VkrMaterial(&system->allocator, config->max_material_count);

  uint64_t hash_size = ((uint64_t)config->max_material_count) * 2ULL;
  if (hash_size > UINT32_MAX) {
    log_fatal("Hash table size overflow for max_material_count %u",
              config->max_material_count);
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
  material->id = 1; // slot 0 -> id 1
  material->generation = system->generation_counter++;
  material->name = "__default";
  material->phong.diffuse_color = vec4_new(1, 1, 1, 1);
  material->phong.specular_color = vec4_new(1, 1, 1, 1);
  material->phong.emission_color = vec4_new(0, 0, 0, 1.0);
  material->phong.shininess = 8.0f;
  // Initialize all texture slots disabled
  for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; i++) {
    material->textures[i].slot = (VkrTextureSlot)i;
    material->textures[i].handle = VKR_TEXTURE_HANDLE_INVALID;
    material->textures[i].enabled = false;
  }
  // Set default diffuse (white texture, not checkerboard)
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      vkr_texture_system_get_default_diffuse_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true;
  material->textures[VKR_TEXTURE_SLOT_NORMAL].handle =
      vkr_texture_system_get_default_normal_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled = true;
  material->textures[VKR_TEXTURE_SLOT_SPECULAR].handle =
      vkr_texture_system_get_default_specular_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_SPECULAR].enabled = true;

  if (system->next_free_index == 0)
    system->next_free_index = 1;

  VkrMaterialHandle handle = {.id = material->id,
                              .generation = material->generation};
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
  material->id = slot + 1;
  material->generation = system->generation_counter++;
  material->name = name_copy;
  material->phong.diffuse_color = diffuse_color;
  material->phong.specular_color = vec4_new(1, 1, 1, 1);
  material->phong.emission_color = vec4_new(0, 0, 0, 1);
  material->phong.shininess = 8.0f;
  material->pipeline_id = VKR_INVALID_ID;

  // Initialize texture slots with defaults
  for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; i++) {
    material->textures[i].slot = (VkrTextureSlot)i;
    material->textures[i].handle = VKR_TEXTURE_HANDLE_INVALID;
    material->textures[i].enabled = false;
  }

  // Set default textures (white diffuse, flat normal, flat specular)
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      vkr_texture_system_get_default_diffuse_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true;
  material->textures[VKR_TEXTURE_SLOT_NORMAL].handle =
      vkr_texture_system_get_default_normal_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled = true;
  material->textures[VKR_TEXTURE_SLOT_SPECULAR].handle =
      vkr_texture_system_get_default_specular_handle(system->texture_system);
  material->textures[VKR_TEXTURE_SLOT_SPECULAR].enabled = true;

  // Register in hash table
  VkrMaterialEntry entry = {
      .id = slot,
      .ref_count = 1,
      .auto_release = true_v,
      .name = name_copy,
  };
  vkr_hash_table_insert_VkrMaterialEntry(&system->material_by_name, name_copy,
                                         entry);

  return (VkrMaterialHandle){.id = material->id,
                             .generation = material->generation};
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

void vkr_material_system_apply_global(VkrMaterialSystem *system,
                                      VkrGlobalMaterialState *global_state,
                                      VkrPipelineDomain domain) {
  assert_log(system != NULL, "System is NULL");
  assert_log(global_state != NULL, "Global state is NULL");

  if (domain == VKR_PIPELINE_DOMAIN_UI) {
    vkr_shader_system_uniform_set(system->shader_system, "view",
                                  &global_state->ui_view);
    vkr_shader_system_uniform_set(system->shader_system, "projection",
                                  &global_state->ui_projection);

  } else {
    vkr_shader_system_uniform_set(system->shader_system, "view",
                                  &global_state->view);
    vkr_shader_system_uniform_set(system->shader_system, "projection",
                                  &global_state->projection);
    vkr_shader_system_uniform_set(system->shader_system, "ambient_color",
                                  &global_state->ambient_color);
    vkr_shader_system_uniform_set(system->shader_system, "view_position",
                                  &global_state->view_position);
    vkr_shader_system_uniform_set(system->shader_system, "render_mode",
                                  &global_state->render_mode);
  }

  vkr_shader_system_apply_global(system->shader_system);
}

void vkr_material_system_apply_instance(VkrMaterialSystem *system,
                                        const VkrMaterial *material,
                                        VkrPipelineDomain domain) {
  assert_log(system != NULL, "System is NULL");
  assert_log(material != NULL, "Material is NULL");

  VkrTextureHandle diffuse_handle =
      material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle;
  VkrTextureHandle default_diffuse =
      vkr_texture_system_get_default_diffuse_handle(system->texture_system);

  VkrTexture *requested_diffuse =
      vkr_texture_system_get_by_handle(system->texture_system, diffuse_handle);
  bool8_t diffuse_valid =
      requested_diffuse && requested_diffuse->handle &&
      requested_diffuse->description.type == VKR_TEXTURE_TYPE_2D &&
      diffuse_handle.id != default_diffuse.id;

  VkrTexture *diffuse_texture = vkr_material_system_resolve_2d_texture(
      system, diffuse_handle, default_diffuse);

  // Domain-specific instance uniforms/samplers
  if (domain == VKR_PIPELINE_DOMAIN_UI) {
    vkr_shader_system_uniform_set(system->shader_system, "diffuse_color",
                                  &material->phong.diffuse_color);

    if (diffuse_texture) {
      vkr_shader_system_sampler_set(system->shader_system, "diffuse_texture",
                                    diffuse_texture->handle);
    }
  } else {
    VkrTextureHandle specular_handle =
        material->textures[VKR_TEXTURE_SLOT_SPECULAR].handle;
    VkrTextureHandle normal_handle =
        material->textures[VKR_TEXTURE_SLOT_NORMAL].handle;

    VkrTextureHandle default_specular =
        vkr_texture_system_get_default_specular_handle(system->texture_system);
    VkrTextureHandle default_normal =
        vkr_texture_system_get_default_normal_handle(system->texture_system);

    VkrTexture *requested_specular = vkr_texture_system_get_by_handle(
        system->texture_system, specular_handle);
    bool8_t specular_valid =
        requested_specular && requested_specular->handle &&
        requested_specular->description.type == VKR_TEXTURE_TYPE_2D &&
        specular_handle.id != default_specular.id;

    VkrTexture *requested_normal =
        vkr_texture_system_get_by_handle(system->texture_system, normal_handle);
    bool8_t normal_valid =
        requested_normal && requested_normal->handle &&
        requested_normal->description.type == VKR_TEXTURE_TYPE_2D &&
        normal_handle.id != default_normal.id;

    VkrTexture *specular_texture = vkr_material_system_resolve_2d_texture(
        system, specular_handle, default_specular);
    VkrTexture *normal_texture = vkr_material_system_resolve_2d_texture(
        system, normal_handle, default_normal);

    // World domain: set all supported Phong-like properties when provided
    vkr_shader_system_uniform_set(system->shader_system, "diffuse_color",
                                  &material->phong.diffuse_color);

    if (diffuse_texture) {
      vkr_shader_system_sampler_set(system->shader_system, "diffuse_texture",
                                    diffuse_texture->handle);
    }

    vkr_shader_system_uniform_set(system->shader_system, "specular_color",
                                  &material->phong.specular_color);

    if (specular_texture) {
      vkr_shader_system_sampler_set(system->shader_system, "specular_texture",
                                    specular_texture->handle);
    }

    vkr_shader_system_uniform_set(system->shader_system, "shininess",
                                  &material->phong.shininess);

    // Build texture flags to tell shader which textures have real data
    // vs default placeholders. Compare against default handles.
    uint32_t texture_flags = 0;

    if (diffuse_valid) {
      texture_flags |= 0x1; // TEXTURE_FLAG_HAS_DIFFUSE
    }
    if (specular_valid) {
      texture_flags |= 0x2; // TEXTURE_FLAG_HAS_SPECULAR
    }
    if (normal_valid) {
      texture_flags |= 0x4; // TEXTURE_FLAG_HAS_NORMAL
    }
    vkr_shader_system_uniform_set(system->shader_system, "texture_flags",
                                  &texture_flags);

    vkr_shader_system_uniform_set(system->shader_system, "emission_color",
                                  &material->phong.emission_color);

    if (normal_texture) {
      vkr_shader_system_sampler_set(system->shader_system, "normal_texture",
                                    normal_texture->handle);
    }

    vkr_material_system_apply_shadow_samplers(system);
  }

  vkr_shader_system_apply_instance(system->shader_system);
}

void vkr_material_system_set_shadow_maps(VkrMaterialSystem *system,
                                         const VkrTextureOpaqueHandle *maps,
                                         uint32_t count, bool8_t enabled) {
  assert_log(system != NULL, "System is NULL");

  if (!enabled || !maps || count == 0) {
    MemZero(system->shadow_maps, sizeof(system->shadow_maps));
    system->shadow_map_count = 0;
    system->shadow_maps_enabled = false_v;
    return;
  }

  if (count > VKR_SHADOW_CASCADE_COUNT_MAX) {
    count = VKR_SHADOW_CASCADE_COUNT_MAX;
  }

  MemZero(system->shadow_maps, sizeof(system->shadow_maps));
  for (uint32_t i = 0; i < count; ++i) {
    system->shadow_maps[i] = maps[i];
  }

  system->shadow_map_count = count;
  system->shadow_maps_enabled = true_v;
}

void vkr_material_system_apply_local(VkrMaterialSystem *system,
                                     VkrLocalMaterialState *local_state) {
  assert_log(system != NULL, "System is NULL");
  assert_log(local_state != NULL, "Local state is NULL");

  vkr_shader_system_uniform_set(system->shader_system, "model",
                                &local_state->model);
  // Set object_id for picking shader (ignored by shaders that don't use it)
  vkr_shader_system_uniform_set(system->shader_system, "object_id",
                                &local_state->object_id);
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
