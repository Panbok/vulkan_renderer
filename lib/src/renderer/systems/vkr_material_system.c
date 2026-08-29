#include "renderer/systems/vkr_material_system.h"

#include "defines.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory_allocator.h"
#include "renderer/systems/vkr_resource_system.h"

#include <stdlib.h>

#define VKR_MATERIAL_SYSTEM_ASYNC_DMEMORY_INITIAL MB(1)
#define VKR_MATERIAL_SYSTEM_ASYNC_DMEMORY_RESERVE MB(16)
#define VKR_MATERIAL_TEXTURE_STREAM_DEFAULT_BUDGET UINT64_MAX

typedef struct VkrGizmoMaterialDef {
  const char *name;
  Vec4 emission;
} VkrGizmoMaterialDef;

VkrMaterialAlphaMode
vkr_material_system_material_alpha_mode(const VkrMaterialSystem *system,
                                        const VkrMaterial *material) {
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
  if (material->material_type != VKR_MATERIAL_TYPE_PBR) {
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
  if (material->id == 0) {
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
  for (uint32_t i = 0; i < VKR_TEXTURE_SLOT_COUNT; i++) {
    material->textures[i].slot = (VkrTextureSlot)i;
    material->textures[i].handle = VKR_TEXTURE_HANDLE_INVALID;
    material->textures[i].enabled = false;
  }
}

vkr_internal void
vkr_material_system_apply_default_surface_textures(VkrMaterialSystem *system,
                                                   VkrMaterial *material) {
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

static void vkr_material_system_remove_texture_stream(VkrMaterialSystem *system,
                                                      uint32_t index) {
  assert_log(system != NULL, "Material system is NULL");
  assert_log(index < system->texture_stream_count,
             "Texture stream index is out of range");
  const uint32_t last = --system->texture_stream_count;
  if (index != last) {
    system->texture_streams[index] = system->texture_streams[last];
  }
  MemZero(&system->texture_streams[last],
          sizeof(system->texture_streams[last]));
}

VkrMaterialTexture
vkr_material_system_get_default_texture(VkrMaterialSystem *system,
                                        VkrTextureSlot slot) {
  assert_log(system != NULL, "Material system is NULL");
  assert_log(system->texture_system != NULL, "Texture system is NULL");
  VkrMaterialTexture texture = {.slot = slot};
  switch (slot) {
  case VKR_TEXTURE_SLOT_DIFFUSE:
  case VKR_TEXTURE_SLOT_OCCLUSION:
    texture.handle =
        vkr_texture_system_get_default_diffuse_handle(system->texture_system);
    texture.enabled = true_v;
    break;
  case VKR_TEXTURE_SLOT_NORMAL:
    texture.handle =
        vkr_texture_system_get_default_normal_handle(system->texture_system);
    texture.enabled = true_v;
    break;
  case VKR_TEXTURE_SLOT_SPECULAR:
    texture.handle =
        vkr_texture_system_get_default_specular_handle(system->texture_system);
    texture.enabled = true_v;
    break;
  case VKR_TEXTURE_SLOT_EMISSION:
    texture.handle =
        vkr_texture_system_get_default_emissive_handle(system->texture_system);
    texture.enabled = true_v;
    break;
  default:
    texture.handle = VKR_TEXTURE_HANDLE_INVALID;
    texture.enabled = false_v;
    break;
  }
  return texture;
}

static bool8_t
vkr_material_system_replace_stream_texture(VkrMaterialSystem *system,
                                           VkrMaterialTextureStream *stream,
                                           VkrMaterialTexture replacement) {
  VkrMaterial *material =
      vkr_material_system_get_by_handle(system, stream->material);
  if (!material) {
    return false_v;
  }
  const VkrMaterialTexture prior = material->textures[stream->slot];
  material->textures[stream->slot] = replacement;
  VkrRendererError publish_error = VKR_RENDERER_ERROR_NONE;
  const bool8_t unpublished =
      vkr_material_system_unpublish(system, stream->material);
  const bool8_t published =
      unpublished &&
      vkr_material_system_publish(system, stream->material, &publish_error);
  if (published) {
    return true_v;
  }
  material->textures[stream->slot] = prior;
  if (unpublished) {
    (void)vkr_material_system_publish(system, stream->material, NULL);
  }
  return false_v;
}

static uint64_t
vkr_material_system_stream_last_used(const VkrMaterialSystem *system,
                                     const VkrMaterialTextureStream *stream) {
  return stream->material.id > 0u &&
                 stream->material.id <= system->config.max_material_count
             ? system
                   ->texture_material_last_used_epochs[stream->material.id - 1u]
             : 0u;
}

static bool8_t vkr_material_system_texture_handle_equal(VkrTextureHandle a,
                                                        VkrTextureHandle b) {
  return a.id == b.id && a.generation == b.generation;
}

static uint32_t
vkr_material_system_resident_texture_users(const VkrMaterialSystem *system,
                                           VkrTextureHandle handle) {
  uint32_t users = 0u;
  for (uint32_t i = 0u; i < system->texture_stream_count; ++i) {
    const VkrMaterialTextureStream *stream = &system->texture_streams[i];
    users += stream->state == VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT &&
                     vkr_material_system_texture_handle_equal(
                         stream->resident_texture, handle)
                 ? 1u
                 : 0u;
  }
  return users;
}

static bool8_t
vkr_material_system_evict_resident_texture(VkrMaterialSystem *system,
                                           VkrTextureHandle texture) {
  if (!system || texture.id == 0u) {
    return false_v;
  }

  uint32_t stream_count = 0u;
  uint64_t resident_bytes = 0u;
  for (uint32_t i = 0u; i < system->texture_stream_count; ++i) {
    VkrMaterialTextureStream *stream = &system->texture_streams[i];
    if (stream->state != VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT ||
        !vkr_material_system_texture_handle_equal(stream->resident_texture,
                                                  texture)) {
      continue;
    }
    if (!vkr_material_system_get_by_handle(system, stream->material)) {
      return false_v;
    }
    resident_bytes = stream->resident_bytes;
    stream_count++;
  }
  if (stream_count == 0u) {
    return false_v;
  }
  const uint32_t ref_count = vkr_texture_system_get_ref_count_by_handle(
      system->texture_system, texture);
  if (ref_count < stream_count) {
    return false_v;
  }

  uint32_t replaced_count = 0u;
  uint32_t release_attempts = 0u;
  for (uint32_t i = 0u; i < system->texture_stream_count; ++i) {
    VkrMaterialTextureStream *stream = &system->texture_streams[i];
    if (stream->state != VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT ||
        !vkr_material_system_texture_handle_equal(stream->resident_texture,
                                                  texture)) {
      continue;
    }
    if (!vkr_material_system_replace_stream_texture(
            system, stream,
            vkr_material_system_get_default_texture(system, stream->slot))) {
      goto rollback;
    }
    replaced_count++;
  }

  for (uint32_t i = 0u; i < stream_count; ++i) {
    if (!vkr_texture_system_get_by_handle(system->texture_system, texture)) {
      break;
    }
    release_attempts++;
    if (!vkr_texture_system_release_by_handle(system->texture_system,
                                              texture)) {
      for (uint32_t restore = 0u; restore < release_attempts; ++restore) {
        vkr_texture_system_add_ref_by_handle(system->texture_system, texture);
      }
      goto rollback;
    }
  }

  for (uint32_t i = 0u; i < system->texture_stream_count; ++i) {
    VkrMaterialTextureStream *stream = &system->texture_streams[i];
    if (stream->state != VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT ||
        !vkr_material_system_texture_handle_equal(stream->resident_texture,
                                                  texture)) {
      continue;
    }
    stream->state = VKR_MATERIAL_TEXTURE_RESIDENCY_EVICTED;
    stream->resident_texture = VKR_TEXTURE_HANDLE_INVALID;
    stream->resident_bytes = 0u;
  }
  system->texture_stream_resident_bytes -= resident_bytes;
  system->texture_stream_resident_count -= stream_count;
  system->texture_stream_evicted_count += stream_count;
  system->texture_stream_evicted_total += stream_count;
  return true_v;

rollback:
  for (uint32_t i = 0u, restored = 0u;
       i < system->texture_stream_count && restored < replaced_count; ++i) {
    VkrMaterialTextureStream *stream = &system->texture_streams[i];
    if (stream->state != VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT ||
        !vkr_material_system_texture_handle_equal(stream->resident_texture,
                                                  texture)) {
      continue;
    }
    (void)vkr_material_system_replace_stream_texture(system, stream,
                                                     (VkrMaterialTexture){
                                                         .handle = texture,
                                                         .slot = stream->slot,
                                                         .enabled = true_v,
                                                     });
    restored++;
  }
  return false_v;
}

static bool8_t
vkr_material_system_evict_to_fit(VkrMaterialSystem *system,
                                 uint64_t incoming_bytes,
                                 VkrTextureHandle protected_texture) {
  if (incoming_bytes > system->texture_stream_budget_bytes) {
    return false_v;
  }
  while (system->texture_stream_resident_bytes >
         system->texture_stream_budget_bytes - incoming_bytes) {
    uint32_t candidate = VKR_INVALID_ID;
    uint64_t oldest_epoch = UINT64_MAX;
    for (uint32_t i = 0u; i < system->texture_stream_count; ++i) {
      VkrMaterialTextureStream *stream = &system->texture_streams[i];
      if (stream->state != VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT ||
          vkr_material_system_texture_handle_equal(stream->resident_texture,
                                                   protected_texture)) {
        continue;
      }
      const uint64_t last_used =
          vkr_material_system_stream_last_used(system, stream);
      const bool8_t unused = last_used < system->texture_stream_epoch;
      const bool8_t candidate_unused =
          candidate != VKR_INVALID_ID &&
          vkr_material_system_stream_last_used(
              system, &system->texture_streams[candidate]) <
              system->texture_stream_epoch;
      if (candidate == VKR_INVALID_ID || (unused && !candidate_unused) ||
          (unused == candidate_unused && last_used < oldest_epoch)) {
        candidate = i;
        oldest_epoch = last_used;
      }
    }
    if (candidate == VKR_INVALID_ID ||
        !vkr_material_system_evict_resident_texture(
            system, system->texture_streams[candidate].resident_texture)) {
      return false_v;
    }
  }
  return true_v;
}

bool8_t vkr_material_system_stream_texture(VkrMaterialSystem *system,
                                           VkrMaterialHandle material,
                                           VkrTextureSlot slot,
                                           const char *path) {
  if (!system || material.id == 0 || slot >= VKR_TEXTURE_SLOT_COUNT || !path ||
      system->texture_stream_count >= system->texture_stream_capacity) {
    return false_v;
  }
  const uint64_t path_length = string_length(path);
  if (path_length == 0u ||
      path_length >= VKR_MATERIAL_TEXTURE_STREAM_PATH_MAX) {
    return false_v;
  }
  VkrMaterialTextureStream *stream =
      &system->texture_streams[system->texture_stream_count++];
  *stream = (VkrMaterialTextureStream){
      .material = material,
      .slot = slot,
      .state = VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED,
      .resident_texture = VKR_TEXTURE_HANDLE_INVALID,
  };
  MemCopy(stream->path, path, path_length);
  stream->path[path_length] = '\0';
  system->texture_stream_queued_count++;
  return true_v;
}

void vkr_material_system_cancel_texture_streams(VkrMaterialSystem *system,
                                                VkrMaterialHandle material) {
  if (!system || material.id == 0) {
    return;
  }
  for (uint32_t i = 0; i < system->texture_stream_count;) {
    VkrMaterialTextureStream *stream = &system->texture_streams[i];
    if (stream->material.id != material.id ||
        stream->material.generation != material.generation) {
      ++i;
      continue;
    }
    String8 path = string8_create_from_cstr((const uint8_t *)stream->path,
                                            string_length(stream->path));
    switch (stream->state) {
    case VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED:
      system->texture_stream_queued_count--;
      break;
    case VKR_MATERIAL_TEXTURE_RESIDENCY_ACTIVE:
      vkr_resource_system_unload(&stream->request, path);
      system->texture_stream_active_count--;
      break;
    case VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT:
      if (vkr_material_system_resident_texture_users(
              system, stream->resident_texture) == 1u) {
        system->texture_stream_resident_bytes -= stream->resident_bytes;
      }
      system->texture_stream_resident_count--;
      break;
    case VKR_MATERIAL_TEXTURE_RESIDENCY_EVICTED:
      system->texture_stream_evicted_count--;
      break;
    }
    vkr_material_system_remove_texture_stream(system, i);
  }
}

void vkr_material_system_begin_texture_residency_frame(
    VkrMaterialSystem *system) {
  if (system) {
    system->texture_stream_epoch++;
  }
}

void vkr_material_system_touch_texture_residency(VkrMaterialSystem *system,
                                                 VkrMaterialHandle material) {
  if (!system || material.id == 0u ||
      material.id > system->config.max_material_count) {
    return;
  }
  uint64_t *last_used =
      &system->texture_material_last_used_epochs[material.id - 1u];
  const bool8_t returned_after_gap =
      *last_used + 1u < system->texture_stream_epoch;
  *last_used = system->texture_stream_epoch;
  if (!returned_after_gap) {
    return;
  }
  for (uint32_t i = 0u; i < system->texture_stream_count; ++i) {
    VkrMaterialTextureStream *stream = &system->texture_streams[i];
    if (stream->material.id == material.id &&
        stream->material.generation == material.generation &&
        stream->state == VKR_MATERIAL_TEXTURE_RESIDENCY_EVICTED) {
      stream->state = VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED;
      system->texture_stream_evicted_count--;
      system->texture_stream_queued_count++;
    }
  }
}

void vkr_material_system_set_texture_residency_budget(VkrMaterialSystem *system,
                                                      uint64_t budget_bytes) {
  if (system) {
    system->texture_stream_budget_bytes = budget_bytes;
    system->texture_stream_budget_user_configured = true_v;
  }
}

void vkr_material_system_set_automatic_texture_residency_budget(
    VkrMaterialSystem *system, uint64_t budget_bytes) {
  if (system && !system->texture_stream_budget_user_configured) {
    system->texture_stream_budget_bytes = budget_bytes;
  }
}

VkrMaterialTextureStreamStats
vkr_material_system_get_texture_stream_stats(const VkrMaterialSystem *system) {
  if (!system) {
    return (VkrMaterialTextureStreamStats){0};
  }
  return (VkrMaterialTextureStreamStats){
      .stream_count = system->texture_stream_count,
      .pending_count = system->texture_stream_queued_count +
                       system->texture_stream_active_count,
      .in_flight_count = system->texture_stream_active_count,
      .resident_count = system->texture_stream_resident_count,
      .evicted_count = system->texture_stream_evicted_count,
      .failed_total = system->texture_stream_failed_total,
  };
}

void vkr_material_system_pump_texture_streams(VkrMaterialSystem *system,
                                              uint32_t max_updates) {
  if (!system || !system->texture_streams || max_updates == 0u) {
    return;
  }
  (void)vkr_material_system_evict_to_fit(system, 0u,
                                         VKR_TEXTURE_HANDLE_INVALID);

  uint32_t updates = 0u;
  for (uint32_t i = 0;
       i < system->texture_stream_count && updates < max_updates;) {
    VkrMaterialTextureStream *stream = &system->texture_streams[i];
    if (stream->state != VKR_MATERIAL_TEXTURE_RESIDENCY_ACTIVE) {
      ++i;
      continue;
    }
    VkrRendererError dependency_error = VKR_RENDERER_ERROR_NONE;
    const VkrResourceLoadState state =
        vkr_resource_system_get_state(&stream->request, &dependency_error);
    if (state == VKR_RESOURCE_LOAD_STATE_PENDING_CPU ||
        state == VKR_RESOURCE_LOAD_STATE_PENDING_DEPENDENCIES ||
        state == VKR_RESOURCE_LOAD_STATE_PENDING_GPU) {
      ++i;
      continue;
    }

    String8 path = string8_create_from_cstr((const uint8_t *)stream->path,
                                            string_length(stream->path));
    VkrMaterial *material =
        vkr_material_system_get_by_handle(system, stream->material);
    VkrResourceHandleInfo resolved = {0};
    VkrTexture *texture = NULL;
    if (state == VKR_RESOURCE_LOAD_STATE_READY && material &&
        vkr_resource_system_try_get_resolved(&stream->request, &resolved) &&
        resolved.type == VKR_RESOURCE_TYPE_TEXTURE &&
        resolved.as.texture.id != 0) {
      texture = vkr_texture_system_get_by_handle(system->texture_system,
                                                 resolved.as.texture);
    }
    if (!texture || texture->description.type != VKR_TEXTURE_TYPE_2D) {
      String8 error_string = vkr_renderer_get_error_string(dependency_error);
      log_warn("Material texture stream %u:%u slot %u '%.*s' failed (%.*s)",
               stream->material.id, stream->material.generation,
               (uint32_t)stream->slot, (int32_t)path.length, path.str,
               (int32_t)error_string.length, error_string.str);
      vkr_resource_system_unload(&stream->request, path);
      system->texture_stream_active_count--;
      system->texture_stream_failed_total++;
      vkr_material_system_remove_texture_stream(system, i);
      updates++;
      continue;
    }

    const VkrTextureHandle texture_handle = resolved.as.texture;
    const uint64_t texture_bytes = texture->resident_bytes;
    const bool8_t already_resident =
        vkr_material_system_resident_texture_users(system, texture_handle) > 0u;
    const uint64_t incoming_bytes = already_resident ? 0u : texture_bytes;
    vkr_texture_system_add_ref_by_handle(system->texture_system,
                                         texture_handle);
    const bool8_t fits = vkr_material_system_evict_to_fit(
        system, incoming_bytes, texture_handle);
    if (!fits) {
      vkr_texture_system_release_by_handle(system->texture_system,
                                           texture_handle);
      vkr_resource_system_unload(&stream->request, path);
      stream->request = (VkrResourceHandleInfo){0};
      stream->state = VKR_MATERIAL_TEXTURE_RESIDENCY_EVICTED;
      system->texture_stream_active_count--;
      system->texture_stream_evicted_count++;
      system->texture_stream_pressure_stalls_total++;
      updates++;
      ++i;
      continue;
    }
    const VkrMaterialTexture replacement = {
        .handle = texture_handle,
        .slot = stream->slot,
        .enabled = true_v,
    };
    if (!vkr_material_system_replace_stream_texture(system, stream,
                                                    replacement)) {
      vkr_texture_system_release_by_handle(system->texture_system,
                                           texture_handle);
      ++i;
      continue;
    }

    vkr_resource_system_unload(&stream->request, path);
    stream->request = (VkrResourceHandleInfo){0};
    stream->state = VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT;
    stream->resident_texture = texture_handle;
    stream->resident_bytes = texture_bytes;
    system->texture_stream_active_count--;
    system->texture_stream_resident_count++;
    system->texture_stream_resident_bytes += incoming_bytes;
    system->texture_stream_applied_total++;
    updates++;
    ++i;
  }

  for (uint32_t i = 0; i < system->texture_stream_count &&
                       updates < max_updates &&
                       system->texture_stream_active_count <
                           VKR_MATERIAL_TEXTURE_STREAM_IN_FLIGHT_MAX &&
                       system->texture_stream_resident_bytes <
                           system->texture_stream_budget_bytes;) {
    VkrMaterialTextureStream *stream = &system->texture_streams[i];
    if (stream->state != VKR_MATERIAL_TEXTURE_RESIDENCY_QUEUED) {
      ++i;
      continue;
    }
    String8 path = string8_create_from_cstr((const uint8_t *)stream->path,
                                            string_length(stream->path));
    VkrRendererError request_error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_resource_system_load(VKR_RESOURCE_TYPE_TEXTURE, path,
                                  &system->async_allocator, &stream->request,
                                  &request_error)) {
      String8 error_string = vkr_renderer_get_error_string(request_error);
      log_warn("Material texture stream %u:%u slot %u '%.*s' failed to queue "
               "(%.*s)",
               stream->material.id, stream->material.generation,
               (uint32_t)stream->slot, (int32_t)path.length, path.str,
               (int32_t)error_string.length, error_string.str);
      if (stream->request.request_id != 0u) {
        vkr_resource_system_unload(&stream->request, path);
      }
      system->texture_stream_queued_count--;
      system->texture_stream_failed_total++;
      vkr_material_system_remove_texture_stream(system, i);
      updates++;
      continue;
    }
    stream->state = VKR_MATERIAL_TEXTURE_RESIDENCY_ACTIVE;
    system->texture_stream_queued_count--;
    system->texture_stream_active_count++;
    updates++;
    ++i;
  }
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
  system->texture_stream_budget_bytes =
      VKR_MATERIAL_TEXTURE_STREAM_DEFAULT_BUDGET;
  const char *budget_mb_env = getenv("VKR_TEXTURE_STREAM_BUDGET_MB");
  if (budget_mb_env && budget_mb_env[0] != '\0') {
    char *end = NULL;
    const unsigned long long budget_mb = strtoull(budget_mb_env, &end, 10);
    if (end && *end == '\0' && budget_mb <= UINT64_MAX / MB(1)) {
      system->texture_stream_budget_bytes = (uint64_t)budget_mb * MB(1);
      system->texture_stream_budget_user_configured = true_v;
    } else {
      log_warn("Ignoring invalid VKR_TEXTURE_STREAM_BUDGET_MB='%s'",
               budget_mb_env);
    }
  }
  system->texture_stream_epoch = 1u;
  system->texture_material_last_used_epochs = vkr_allocator_alloc(
      &system->allocator,
      (uint64_t)config->max_material_count *
          sizeof(*system->texture_material_last_used_epochs),
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  system->texture_stream_capacity = VKR_MATERIAL_TEXTURE_STREAM_CAPACITY;
  system->texture_streams =
      vkr_allocator_alloc(&system->async_allocator,
                          (uint64_t)system->texture_stream_capacity *
                              sizeof(VkrMaterialTextureStream),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!system->texture_streams || !system->texture_material_last_used_epochs) {
    vkr_material_system_shutdown(system);
    return false_v;
  }
  MemZero(system->texture_streams, (uint64_t)system->texture_stream_capacity *
                                       sizeof(VkrMaterialTextureStream));
  MemZero(system->texture_material_last_used_epochs,
          (uint64_t)config->max_material_count *
              sizeof(*system->texture_material_last_used_epochs));
  if (system->texture_stream_budget_bytes == UINT64_MAX) {
    log_info("Material texture residency budget: unlimited, %u in flight",
             VKR_MATERIAL_TEXTURE_STREAM_IN_FLIGHT_MAX);
  } else {
    log_info("Material texture residency budget: %llu MiB, %u in flight",
             (unsigned long long)(system->texture_stream_budget_bytes / MB(1)),
             VKR_MATERIAL_TEXTURE_STREAM_IN_FLIGHT_MAX);
  }

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
  while (system->texture_stream_count > 0u) {
    VkrMaterialTextureStream *stream =
        &system->texture_streams[system->texture_stream_count - 1u];
    String8 path = string8_create_from_cstr((const uint8_t *)stream->path,
                                            string_length(stream->path));
    if (stream->state == VKR_MATERIAL_TEXTURE_RESIDENCY_ACTIVE) {
      vkr_resource_system_unload(&stream->request, path);
      system->texture_stream_active_count--;
    } else if (stream->state == VKR_MATERIAL_TEXTURE_RESIDENCY_RESIDENT) {
      if (vkr_material_system_resident_texture_users(
              system, stream->resident_texture) == 1u) {
        system->texture_stream_resident_bytes -= stream->resident_bytes;
      }
      system->texture_stream_resident_count--;
    }
    system->texture_stream_count--;
  }
  if (system->texture_streams) {
    vkr_allocator_free(&system->async_allocator, system->texture_streams,
                       (uint64_t)system->texture_stream_capacity *
                           sizeof(VkrMaterialTextureStream),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    system->texture_streams = NULL;
  }
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
                                      VkrTextureOpaqueHandle prefilter_map,
                                      bool8_t enabled, float32_t intensity,
                                      float32_t diffuse_intensity,
                                      float32_t specular_intensity) {
  assert_log(system != NULL, "System is NULL");

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
  if (handle.id == 0)
    return NULL;
  uint32_t index = handle.id - 1;
  if (index >= system->materials.length)
    return NULL;
  VkrMaterial *material = &system->materials.data[index];
  return (material->generation == handle.generation) ? material : NULL;
}

VkrMaterial *vkr_material_system_get_live(VkrMaterialSystem *system,
                                          VkrMaterialHandle handle) {
  return &system->materials.data[handle.id - 1u];
}
