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

vkr_internal void vkr_material_system_sampler_set_optional(
    VkrMaterialSystem *system, const char *name, VkrTextureOpaqueHandle value);
vkr_internal void
vkr_material_system_uniform_set_optional(VkrMaterialSystem *system,
                                         const char *name, const void *value);

vkr_internal VkrTextureOpaqueHandle
vkr_material_system_get_shadow_fallback(VkrMaterialSystem *system) {
  if (!system || !system->texture_system) {
    return NULL;
  }
  VkrTexture *fallback = vkr_texture_system_get_default(system->texture_system);
  return fallback ? fallback->handle : NULL;
}

/**
 * @brief Returns a depth-texture fallback for shadow-map bindings.
 *
 * The world shader uses a comparison sampler for `shadow_map`. If it is bound
 * to a color texture (e.g. the default checkerboard), Vulkan validation will
 * report that the image format does not support depth comparison sampling. To
 * keep descriptors valid when shadows are disabled, reuse the last depth map.
 */
vkr_internal VkrTextureOpaqueHandle
vkr_material_system_get_shadow_depth_fallback(VkrMaterialSystem *system) {
  if (!system || !system->shadow_map) {
    return NULL;
  }

  return system->shadow_map;
}

vkr_internal void
vkr_material_system_apply_shadow_samplers(VkrMaterialSystem *system) {
  vkr_local_persist const char *k_shadow_sampler = "shadow_map";

  VkrTextureOpaqueHandle fallback =
      vkr_material_system_get_shadow_depth_fallback(system);
  if (!fallback) {
    fallback = vkr_material_system_get_shadow_fallback(system);
  }

  VkrTextureOpaqueHandle map = fallback;
  if (system->shadow_maps_enabled && system->shadow_map) {
    map = system->shadow_map;
  }
  vkr_shader_system_sampler_set(system->shader_system, k_shadow_sampler, map);
}

vkr_internal void
vkr_material_system_apply_ibl_samplers(VkrMaterialSystem *system) {
  if (!system) {
    return;
  }

  VkrTextureOpaqueHandle irradiance_map =
      system->ibl_probe_slots[0].irradiance_map
          ? system->ibl_probe_slots[0].irradiance_map
          : system->ibl_irradiance_map;
  VkrTextureOpaqueHandle prefilter_map =
      system->ibl_probe_slots[0].prefilter_map
          ? system->ibl_probe_slots[0].prefilter_map
          : system->ibl_prefilter_map;
  VkrTextureOpaqueHandle irradiance_map_b =
      system->ibl_probe_slots[1].irradiance_map
          ? system->ibl_probe_slots[1].irradiance_map
          : irradiance_map;
  VkrTextureOpaqueHandle prefilter_map_b =
      system->ibl_probe_slots[1].prefilter_map
          ? system->ibl_probe_slots[1].prefilter_map
          : prefilter_map;

  if (irradiance_map) {
    vkr_material_system_sampler_set_optional(system, "irradiance_map",
                                             irradiance_map);
  }
  if (prefilter_map) {
    vkr_material_system_sampler_set_optional(system, "prefilter_map",
                                             prefilter_map);
  }
  if (system->ibl_brdf_lut) {
    vkr_material_system_sampler_set_optional(system, "brdf_lut",
                                             system->ibl_brdf_lut);
  }

  if (irradiance_map_b) {
    vkr_material_system_sampler_set_optional(system, "irradiance_map_b",
                                             irradiance_map_b);
  }
  if (prefilter_map_b) {
    vkr_material_system_sampler_set_optional(system, "prefilter_map_b",
                                             prefilter_map_b);
  }
}

vkr_internal void
vkr_material_system_apply_ibl_probe_uniforms(VkrMaterialSystem *system) {
  if (!system) {
    return;
  }

  Vec4 center_blend[2] = {0};
  Vec4 extents_weight[2] = {0};
  Vec4 intensity_box[2] = {0};

  for (uint32_t i = 0; i < 2u; ++i) {
    const VkrMaterialIblProbeSlot *slot = &system->ibl_probe_slots[i];
    center_blend[i] = vec4_new(slot->center.x, slot->center.y, slot->center.z,
                               slot->blend_distance);
    extents_weight[i] = vec4_new(slot->extents.x, slot->extents.y,
                                 slot->extents.z, slot->weight);
    intensity_box[i] = vec4_new(slot->intensity, slot->diffuse_intensity,
                                slot->specular_intensity,
                                slot->box_projection_enabled ? 1.0f : 0.0f);
  }

  vkr_material_system_uniform_set_optional(system, "ibl_probe0_center_blend",
                                           &center_blend[0]);
  vkr_material_system_uniform_set_optional(system, "ibl_probe0_extents_weight",
                                           &extents_weight[0]);
  vkr_material_system_uniform_set_optional(system, "ibl_probe0_intensity_box",
                                           &intensity_box[0]);
  vkr_material_system_uniform_set_optional(system, "ibl_probe1_center_blend",
                                           &center_blend[1]);
  vkr_material_system_uniform_set_optional(system, "ibl_probe1_extents_weight",
                                           &extents_weight[1]);
  vkr_material_system_uniform_set_optional(system, "ibl_probe1_intensity_box",
                                           &intensity_box[1]);
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

vkr_internal VkrTextureHandle vkr_material_system_default_texture_for_slot(
    VkrMaterialSystem *system, VkrTextureSlot slot) {
  if (!system || !system->texture_system) {
    return VKR_TEXTURE_HANDLE_INVALID;
  }

  switch (slot) {
  case VKR_TEXTURE_SLOT_NORMAL:
    return vkr_texture_system_get_default_normal_handle(system->texture_system);
  case VKR_TEXTURE_SLOT_SPECULAR:
  case VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS:
    return vkr_texture_system_get_default_specular_handle(
        system->texture_system);
  case VKR_TEXTURE_SLOT_OCCLUSION:
  case VKR_TEXTURE_SLOT_EMISSION:
  case VKR_TEXTURE_SLOT_DIFFUSE:
  default:
    return vkr_texture_system_get_default_diffuse_handle(
        system->texture_system);
  }
}

vkr_internal void
vkr_material_system_uniform_set_optional(VkrMaterialSystem *system,
                                         const char *name, const void *value) {
  if (!system || !system->shader_system ||
      !system->shader_system->current_shader || !name || !value) {
    return;
  }

  if (vkr_shader_system_uniform_index(
          system->shader_system, system->shader_system->current_shader, name) ==
      VKR_SHADER_INVALID_UNIFORM_INDEX) {
    return;
  }

  vkr_shader_system_uniform_set(system->shader_system, name, value);
}

vkr_internal void vkr_material_system_sampler_set_optional(
    VkrMaterialSystem *system, const char *name, VkrTextureOpaqueHandle value) {
  if (!system || !system->shader_system ||
      !system->shader_system->current_shader || !name || !value) {
    return;
  }

  if (vkr_shader_system_uniform_index(
          system->shader_system, system->shader_system->current_shader, name) ==
      VKR_SHADER_INVALID_UNIFORM_INDEX) {
    return;
  }

  vkr_shader_system_sampler_set(system->shader_system, name, value);
}

vkr_internal VkrMaterialAlphaMode vkr_material_system_material_alpha_mode(
    const VkrMaterialSystem *system, const VkrMaterial *material) {
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
  material->pipeline_id = VKR_INVALID_ID;

  vkr_material_system_reset_texture_slots(material);
  vkr_material_system_apply_default_surface_textures(system, material);
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
  system->shader_system = shader_system;
  system->ibl_intensity = 1.0f;
  system->ibl_diffuse_intensity = 1.0f;
  system->ibl_specular_intensity = 1.0f;
  for (uint32_t i = 0; i < 2u; ++i) {
    system->ibl_probe_slots[i] = (VkrMaterialIblProbeSlot){
        .irradiance_map = NULL,
        .prefilter_map = NULL,
        .center = {0},
        .extents = {0},
        .blend_distance = 0.0f,
        .weight = (i == 0u) ? 1.0f : 0.0f,
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

void vkr_material_system_apply_global(
    VkrMaterialSystem *system, const VkrGlobalMaterialState *global_state,
    VkrPipelineDomain domain) {
  assert_log(system != NULL, "System is NULL");
  assert_log(global_state != NULL, "Global state is NULL");

  bool8_t world_globals = (domain == VKR_PIPELINE_DOMAIN_WORLD) ||
                          (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT) ||
                          (domain == VKR_PIPELINE_DOMAIN_WORLD_OVERLAY);

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
    if (world_globals) {
      vkr_material_system_uniform_set_optional(system, "ambient_color",
                                               &global_state->ambient_color);
      vkr_material_system_uniform_set_optional(system, "view_position",
                                               &global_state->view_position);
      vkr_material_system_uniform_set_optional(system, "render_mode",
                                               &global_state->render_mode);
      uint32_t ibl_enabled = system->ibl_enabled ? 1u : 0u;
      vkr_material_system_uniform_set_optional(system, "ibl_enabled",
                                               &ibl_enabled);
      vkr_material_system_uniform_set_optional(system, "ibl_intensity",
                                               &system->ibl_intensity);
      vkr_material_system_uniform_set_optional(system, "ibl_diffuse_intensity",
                                               &system->ibl_diffuse_intensity);
      vkr_material_system_uniform_set_optional(system, "ibl_specular_intensity",
                                               &system->ibl_specular_intensity);
    }
  }

  vkr_shader_system_apply_global(system->shader_system);
}

void vkr_material_system_apply_instance(VkrMaterialSystem *system,
                                        const VkrMaterial *material,
                                        VkrPipelineDomain domain) {
  assert_log(system != NULL, "System is NULL");
  assert_log(material != NULL, "Material is NULL");

  // A material owns the complete descriptor state for its shader. Optional
  // slots must not inherit images from a previously applied material.
  vkr_shader_system_reset_material_state(system->shader_system);

  VkrTextureHandle diffuse_handle =
      material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle;
  VkrTextureHandle default_diffuse =
      vkr_material_system_default_texture_for_slot(system,
                                                   VKR_TEXTURE_SLOT_DIFFUSE);
  VkrTextureHandle normal_handle =
      material->textures[VKR_TEXTURE_SLOT_NORMAL].handle;
  VkrTextureHandle default_normal =
      vkr_material_system_default_texture_for_slot(system,
                                                   VKR_TEXTURE_SLOT_NORMAL);

  VkrTexture *diffuse_texture = vkr_material_system_resolve_2d_texture(
      system, diffuse_handle, default_diffuse);
  VkrTexture *normal_texture = vkr_material_system_resolve_2d_texture(
      system, normal_handle, default_normal);

  VkrTexture *requested_diffuse =
      vkr_texture_system_get_by_handle(system->texture_system, diffuse_handle);
  bool8_t diffuse_valid =
      requested_diffuse && requested_diffuse->handle &&
      requested_diffuse->description.type == VKR_TEXTURE_TYPE_2D &&
      diffuse_handle.id != default_diffuse.id;

  if (domain == VKR_PIPELINE_DOMAIN_UI) {
    Vec4 ui_color = material->material_type == VKR_MATERIAL_TYPE_PBR
                        ? material->pbr.base_color
                        : material->phong.diffuse_color;
    vkr_shader_system_uniform_set(system->shader_system, "diffuse_color",
                                  &ui_color);

    if (diffuse_texture) {
      vkr_shader_system_sampler_set(system->shader_system, "diffuse_texture",
                                    diffuse_texture->handle);
    }
  } else {
    if (material->material_type == VKR_MATERIAL_TYPE_PBR) {
      VkrTextureHandle mr_handle =
          material->textures[VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS].handle;
      VkrTextureHandle occlusion_handle =
          material->textures[VKR_TEXTURE_SLOT_OCCLUSION].handle;
      VkrTextureHandle emissive_handle =
          material->textures[VKR_TEXTURE_SLOT_EMISSION].handle;

      VkrTextureHandle default_mr =
          vkr_material_system_default_texture_for_slot(
              system, VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS);
      VkrTextureHandle default_occlusion =
          vkr_material_system_default_texture_for_slot(
              system, VKR_TEXTURE_SLOT_OCCLUSION);
      VkrTextureHandle default_emissive =
          vkr_material_system_default_texture_for_slot(
              system, VKR_TEXTURE_SLOT_EMISSION);

      VkrTexture *mr_texture =
          vkr_material_system_resolve_2d_texture(system, mr_handle, default_mr);
      VkrTexture *occlusion_texture = vkr_material_system_resolve_2d_texture(
          system, occlusion_handle, default_occlusion);
      VkrTexture *emissive_texture = vkr_material_system_resolve_2d_texture(
          system, emissive_handle, default_emissive);

      VkrTexture *requested_mr =
          vkr_texture_system_get_by_handle(system->texture_system, mr_handle);
      VkrTexture *requested_occlusion = vkr_texture_system_get_by_handle(
          system->texture_system, occlusion_handle);
      VkrTexture *requested_emissive = vkr_texture_system_get_by_handle(
          system->texture_system, emissive_handle);
      VkrTexture *requested_normal = vkr_texture_system_get_by_handle(
          system->texture_system, normal_handle);

      bool8_t normal_valid =
          requested_normal && requested_normal->handle &&
          requested_normal->description.type == VKR_TEXTURE_TYPE_2D &&
          normal_handle.id != default_normal.id;
      bool8_t mr_valid =
          requested_mr && requested_mr->handle &&
          requested_mr->description.type == VKR_TEXTURE_TYPE_2D &&
          mr_handle.id != default_mr.id;
      bool8_t occlusion_valid =
          requested_occlusion && requested_occlusion->handle &&
          requested_occlusion->description.type == VKR_TEXTURE_TYPE_2D &&
          occlusion_handle.id != default_occlusion.id;
      bool8_t emissive_valid =
          requested_emissive && requested_emissive->handle &&
          requested_emissive->description.type == VKR_TEXTURE_TYPE_2D &&
          emissive_handle.id != default_emissive.id;

      vkr_material_system_uniform_set_optional(system, "base_color",
                                               &material->pbr.base_color);
      vkr_material_system_uniform_set_optional(system, "metallic",
                                               &material->pbr.metallic);
      vkr_material_system_uniform_set_optional(system, "roughness",
                                               &material->pbr.roughness);
      vkr_material_system_uniform_set_optional(system, "normal_scale",
                                               &material->pbr.normal_scale);
      vkr_material_system_uniform_set_optional(
          system, "occlusion_strength", &material->pbr.occlusion_strength);
      vkr_material_system_uniform_set_optional(system, "emissive_factor",
                                               &material->pbr.emissive_factor);

      uint32_t alpha_mode =
          (uint32_t)vkr_material_system_material_alpha_mode(system, material);
      vkr_material_system_uniform_set_optional(system, "alpha_mode",
                                               &alpha_mode);

      vkr_material_system_uniform_set_optional(system, "diffuse_color",
                                               &material->pbr.base_color);
      vkr_material_system_uniform_set_optional(system, "specular_color",
                                               &material->phong.specular_color);
      vkr_material_system_uniform_set_optional(system, "shininess",
                                               &material->phong.shininess);
      vkr_material_system_uniform_set_optional(system, "emission_color",
                                               &material->pbr.emissive_factor);

      if (domain == VKR_PIPELINE_DOMAIN_WORLD ||
          domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT ||
          domain == VKR_PIPELINE_DOMAIN_WORLD_OVERLAY ||
          domain == VKR_PIPELINE_DOMAIN_SHADOW) {
        float32_t alpha_cutoff =
            vkr_material_system_material_alpha_cutoff(system, material);
        vkr_shader_system_uniform_set(system->shader_system, "alpha_cutoff",
                                      &alpha_cutoff);
      }

      uint32_t texture_flags = 0;
      if (diffuse_valid) {
        texture_flags |= 0x1u;
      }
      if (normal_valid) {
        texture_flags |= 0x2u;
      }
      if (mr_valid) {
        texture_flags |= 0x4u;
      }
      if (occlusion_valid) {
        texture_flags |= 0x8u;
      }
      if (emissive_valid) {
        texture_flags |= 0x10u;
      }
      vkr_shader_system_uniform_set(system->shader_system, "texture_flags",
                                    &texture_flags);

      if (diffuse_texture) {
        vkr_material_system_sampler_set_optional(system, "base_color_texture",
                                                 diffuse_texture->handle);
        vkr_material_system_sampler_set_optional(system, "diffuse_texture",
                                                 diffuse_texture->handle);
      }
      if (normal_texture) {
        vkr_material_system_sampler_set_optional(system, "normal_texture",
                                                 normal_texture->handle);
      }
      if (mr_texture) {
        vkr_material_system_sampler_set_optional(
            system, "metallic_roughness_texture", mr_texture->handle);
      }
      if (occlusion_texture) {
        vkr_material_system_sampler_set_optional(system, "occlusion_texture",
                                                 occlusion_texture->handle);
      }
      if (emissive_texture) {
        vkr_material_system_sampler_set_optional(system, "emissive_texture",
                                                 emissive_texture->handle);
      }
      vkr_material_system_apply_ibl_probe_uniforms(system);
      vkr_material_system_apply_ibl_samplers(system);
    } else {
      VkrTextureHandle specular_handle =
          material->textures[VKR_TEXTURE_SLOT_SPECULAR].handle;
      VkrTextureHandle default_specular =
          vkr_material_system_default_texture_for_slot(
              system, VKR_TEXTURE_SLOT_SPECULAR);

      VkrTexture *specular_texture = vkr_material_system_resolve_2d_texture(
          system, specular_handle, default_specular);
      VkrTexture *requested_specular = vkr_texture_system_get_by_handle(
          system->texture_system, specular_handle);
      VkrTexture *requested_normal = vkr_texture_system_get_by_handle(
          system->texture_system, normal_handle);

      bool8_t specular_valid =
          requested_specular && requested_specular->handle &&
          requested_specular->description.type == VKR_TEXTURE_TYPE_2D &&
          specular_handle.id != default_specular.id;
      bool8_t normal_valid =
          requested_normal && requested_normal->handle &&
          requested_normal->description.type == VKR_TEXTURE_TYPE_2D &&
          normal_handle.id != default_normal.id;

      vkr_shader_system_uniform_set(system->shader_system, "diffuse_color",
                                    &material->phong.diffuse_color);
      vkr_shader_system_uniform_set(system->shader_system, "specular_color",
                                    &material->phong.specular_color);
      vkr_shader_system_uniform_set(system->shader_system, "shininess",
                                    &material->phong.shininess);
      vkr_shader_system_uniform_set(system->shader_system, "emission_color",
                                    &material->phong.emission_color);

      if (domain == VKR_PIPELINE_DOMAIN_WORLD ||
          domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT ||
          domain == VKR_PIPELINE_DOMAIN_WORLD_OVERLAY ||
          domain == VKR_PIPELINE_DOMAIN_SHADOW) {
        float32_t alpha_cutoff =
            vkr_material_system_material_alpha_cutoff(system, material);
        vkr_shader_system_uniform_set(system->shader_system, "alpha_cutoff",
                                      &alpha_cutoff);
      }

      uint32_t texture_flags = 0;
      if (diffuse_valid) {
        texture_flags |= 0x1u;
      }
      if (specular_valid) {
        texture_flags |= 0x2u;
      }
      if (normal_valid) {
        texture_flags |= 0x4u;
      }
      vkr_shader_system_uniform_set(system->shader_system, "texture_flags",
                                    &texture_flags);

      if (diffuse_texture) {
        vkr_shader_system_sampler_set(system->shader_system, "diffuse_texture",
                                      diffuse_texture->handle);
      }
      if (specular_texture) {
        vkr_shader_system_sampler_set(system->shader_system, "specular_texture",
                                      specular_texture->handle);
      }
      if (normal_texture) {
        vkr_shader_system_sampler_set(system->shader_system, "normal_texture",
                                      normal_texture->handle);
      }
    }

    vkr_material_system_apply_shadow_samplers(system);
  }

  vkr_shader_system_apply_instance(system->shader_system);
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
                                      VkrTextureOpaqueHandle brdf_lut,
                                      bool8_t enabled, float32_t intensity,
                                      float32_t diffuse_intensity,
                                      float32_t specular_intensity) {
  assert_log(system != NULL, "System is NULL");

  system->ibl_irradiance_map = irradiance_map;
  system->ibl_prefilter_map = prefilter_map;
  system->ibl_brdf_lut = brdf_lut;
  system->ibl_enabled = enabled ? true_v : false_v;
  system->ibl_intensity = intensity;
  system->ibl_diffuse_intensity = diffuse_intensity;
  system->ibl_specular_intensity = specular_intensity;
}

void vkr_material_system_set_ibl_probe_slots(
    VkrMaterialSystem *system, const VkrMaterialIblProbeSlot slots[2]) {
  assert_log(system != NULL, "System is NULL");

  if (!slots) {
    for (uint32_t i = 0; i < 2u; ++i) {
      system->ibl_probe_slots[i] = (VkrMaterialIblProbeSlot){
          .irradiance_map = NULL,
          .prefilter_map = NULL,
          .center = {0},
          .extents = {0},
          .blend_distance = 0.0f,
          .weight = (i == 0u) ? 1.0f : 0.0f,
          .intensity = 1.0f,
          .diffuse_intensity = 1.0f,
          .specular_intensity = 1.0f,
          .box_projection_enabled = false_v,
      };
    }
    return;
  }

  for (uint32_t i = 0; i < 2u; ++i) {
    system->ibl_probe_slots[i] = slots[i];
    if (system->ibl_probe_slots[i].blend_distance < 0.0f) {
      system->ibl_probe_slots[i].blend_distance = 0.0f;
    }
    if (system->ibl_probe_slots[i].weight < 0.0f) {
      system->ibl_probe_slots[i].weight = 0.0f;
    }
  }
}

void vkr_material_system_apply_local(VkrMaterialSystem *system,
                                     VkrLocalMaterialState *local_state) {
  assert_log(system != NULL, "System is NULL");
  assert_log(local_state != NULL, "Local state is NULL");

  vkr_shader_system_uniform_set(system->shader_system, "model",
                                &local_state->model);
  // Set object_id for picking shader (ignored by shaders that don't use it)
  vkr_material_system_uniform_set_optional(system, "object_id",
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
