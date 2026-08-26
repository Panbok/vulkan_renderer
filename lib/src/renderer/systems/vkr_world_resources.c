/**
 * @file vkr_world_resources.c
 * @brief Shared world pipelines, HDR/IBL state, and 3D text resources.
 */

#include "renderer/systems/vkr_world_resources.h"

#include <stdio.h>

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_render_packet.h"

#define VKR_WORLD_RESOURCES_MAX_TEXTS 16
#define VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE VKR_IBL_IRRADIANCE_SIZE
#define VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE VKR_IBL_PREFILTER_SIZE
#define VKR_WORLD_RESOURCES_IBL_BRDF_SIZE VKR_IBL_BRDF_SIZE
#define VKR_WORLD_RESOURCES_IBL_RENDERPASS_NAME                                \
  "Renderpass.Builtin.IBL.Convolution"

vkr_internal bool8_t vkr_world_resources_ensure_text_slot(
    VkrWorldResources *resources, uint32_t text_id,
    VkrWorldTextSlot **out_slot) {
  if (!resources || !out_slot || !resources->text_slots.data) {
    return false_v;
  }
  if (text_id >= resources->text_slots.length) {
    log_error("World text id %u exceeds max (%llu)", text_id,
              (unsigned long long)resources->text_slots.length);
    return false_v;
  }

  *out_slot = &resources->text_slots.data[text_id];
  return true_v;
}

vkr_internal VkrWorldTextSlot *
vkr_world_resources_get_text_slot(VkrWorldResources *resources,
                                  uint32_t text_id) {
  if (!resources || !resources->text_slots.data ||
      text_id >= resources->text_slots.length) {
    return NULL;
  }

  VkrWorldTextSlot *slot = &resources->text_slots.data[text_id];
  return slot->active ? slot : NULL;
}

vkr_internal bool8_t vkr_world_resources_texture_is_valid(
    VkrTextureSystem *texture_system, VkrTextureHandle handle,
    VkrTextureType expected_type) {
  if (!texture_system || handle.id == 0) {
    return false_v;
  }

  VkrTexture *texture =
      vkr_texture_system_get_by_handle(texture_system, handle);
  return texture && texture->handle &&
         texture->description.type == expected_type;
}

vkr_internal VkrTextureOpaqueHandle vkr_world_resources_resolve_backend_texture(
    VkrTextureSystem *texture_system, VkrTextureHandle handle,
    VkrTextureType expected_type) {
  if (!texture_system || handle.id == 0) {
    return NULL;
  }

  VkrTexture *texture =
      vkr_texture_system_get_by_handle(texture_system, handle);
  if (!texture || !texture->handle ||
      texture->description.type != expected_type) {
    return NULL;
  }

  return texture->handle;
}

vkr_internal bool8_t vkr_world_resources_release_texture(
    VkrTextureSystem *texture_system, VkrTextureHandle *handle) {
  if (!texture_system || !handle || handle->id == 0) {
    return true_v;
  }

  if (!vkr_texture_system_release_by_handle(texture_system, *handle)) {
    return false_v;
  }
  *handle = VKR_TEXTURE_HANDLE_INVALID;
  return true_v;
}

vkr_internal bool8_t
vkr_world_resources_has_retained_ibl_publisher(const RendererFrontend *rf) {
  return rf && rf->asset_publisher.publish_writable_texture &&
         rf->asset_publisher.bake_ibl_cubemap &&
         rf->asset_publisher.bake_hdr_environment;
}

vkr_internal uint32_t vkr_world_resources_calculate_mip_count(uint32_t size) {
  uint32_t mips = 1u;
  while (size > 1u) {
    size >>= 1u;
    mips++;
  }
  return mips;
}

vkr_internal uint32_t
vkr_world_resources_ibl_mip_limit(const VkrWorldResources *resources) {
  return resources ? Min(resources->hdr_ibl_max_mip_levels,
                         (uint32_t)VKR_IBL_PREFILTER_MIP_COUNT)
                   : 0u;
}

vkr_internal bool8_t vkr_world_resources_create_writable_cube_texture(
    RendererFrontend *rf, String8 name, uint32_t size, bool8_t with_mips,
    VkrTextureFormat format, VkrTextureHandle *out_handle) {
  if (!rf || !name.str || !out_handle || size == 0) {
    return false_v;
  }

  VkrTextureDescription desc = {
      .width = size,
      .height = size,
      .channels = 4,
      .type = VKR_TEXTURE_TYPE_CUBE_MAP,
      .format = format,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = with_mips ? VKR_MIP_FILTER_LINEAR : VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
  };

  VkrRendererError texture_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_create_writable(&rf->texture_system, name, &desc,
                                          out_handle, &texture_err)) {
    String8 err = vkr_renderer_get_error_string(texture_err);
    log_warn("World resources: failed to create writable cubemap '%.*s': %s",
             (int)name.length, name.str, string8_cstr(&err));
    return false_v;
  }

  return true_v;
}

vkr_internal bool8_t vkr_world_resources_create_writable_2d_texture(
    RendererFrontend *rf, String8 name, uint32_t width, uint32_t height,
    VkrTextureFormat format, VkrTextureHandle *out_handle) {
  if (!rf || !name.str || !out_handle || width == 0u || height == 0u) {
    return false_v;
  }

  VkrTextureDescription desc = {
      .width = width,
      .height = height,
      .channels = 4u,
      .type = VKR_TEXTURE_TYPE_2D,
      .format = format,
      .allocation_owner = VKR_GPU_ALLOCATION_OWNER_TEXTURE,
      .sample_count = VKR_SAMPLE_COUNT_1,
      .properties = vkr_texture_property_flags_create(),
      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
      .min_filter = VKR_FILTER_LINEAR,
      .mag_filter = VKR_FILTER_LINEAR,
      .mip_filter = VKR_MIP_FILTER_NONE,
      .anisotropy_enable = false_v,
  };

  VkrRendererError texture_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_create_writable(&rf->texture_system, name, &desc,
                                          out_handle, &texture_error)) {
    String8 error_string = vkr_renderer_get_error_string(texture_error);
    log_warn("World resources: failed to create writable texture '%.*s': %s",
             (int)name.length, name.str, string8_cstr(&error_string));
    return false_v;
  }
  return true_v;
}

bool8_t vkr_world_resources_init(RendererFrontend *rf,
                                 VkrWorldResources *resources) {
  if (!rf || !resources) {
    return false_v;
  }
  MemZero(resources, sizeof(*resources));
  resources->ibl_fallback_source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_intensity = 1.0f;
  resources->ibl_active_diffuse_intensity = 1.0f;
  resources->ibl_active_specular_intensity = 1.0f;
  resources->hdr_ibl_max_cube_extent = VKR_IBL_PREFILTER_SIZE;
  resources->hdr_ibl_max_mip_levels = VKR_IBL_PREFILTER_MIP_COUNT;
  resources->text_slots = array_create_VkrWorldTextSlot(
      &rf->allocator, VKR_WORLD_RESOURCES_MAX_TEXTS);
  if (!resources->text_slots.data) {
    return false_v;
  }
  MemZero(resources->text_slots.data,
          sizeof(VkrWorldTextSlot) * (uint64_t)resources->text_slots.length);
  resources->initialized = true_v;
  return true_v;
}

void vkr_world_resources_release_scene_environment_targets(RendererFrontend *rf,
                                                           VkrScene *scene) {
  (void)rf;
  (void)scene;
}

vkr_internal void
vkr_world_resources_fail_scene_environment(RendererFrontend *rf,
                                           VkrScene *scene) {
  if (!rf || !scene) {
    return;
  }

  VkrSceneEnvironment *environment = &scene->environment;
  vkr_world_resources_release_scene_environment_targets(rf, scene);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &environment->prefilter_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &environment->irradiance_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &environment->source_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &environment->delivery_equirect);
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
}

vkr_internal void vkr_world_resources_retain_first_scene_environment_as_default(
    RendererFrontend *rf, VkrWorldResources *resources,
    const VkrSceneEnvironment *environment) {
  if (resources->ibl_default_ready) {
    return;
  }

  vkr_texture_system_add_ref_by_handle(&rf->texture_system,
                                       environment->source_cubemap);
  vkr_texture_system_add_ref_by_handle(&rf->texture_system,
                                       environment->irradiance_cubemap);
  vkr_texture_system_add_ref_by_handle(&rf->texture_system,
                                       environment->prefilter_cubemap);
  resources->ibl_fallback_source_cubemap = environment->source_cubemap;
  resources->ibl_fallback_irradiance_cubemap = environment->irradiance_cubemap;
  resources->ibl_fallback_prefilter_cubemap = environment->prefilter_cubemap;
  resources->ibl_default_ready = true_v;
}

vkr_internal bool8_t vkr_world_resources_prepare_published_environment(
    RendererFrontend *rf, VkrWorldResources *resources, VkrScene *scene) {
  VkrSceneEnvironment *environment = &scene->environment;
  if (environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT) {
    VkrTexture *delivery = vkr_texture_system_get_by_handle(
        &rf->texture_system, environment->delivery_equirect);
    if (!delivery || !delivery->handle ||
        delivery->description.type != VKR_TEXTURE_TYPE_2D ||
        delivery->description.format !=
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT ||
        !rf->asset_publisher.bake_hdr_environment ||
        !vkr_ibl_derive_cubemap_size(
            delivery->description.width, delivery->description.height,
            resources->hdr_ibl_max_cube_extent,
            vkr_world_resources_ibl_mip_limit(resources),
            &environment->source_face_size, &environment->source_mip_count)) {
      goto failed;
    }
  } else if (environment->source_kind == VKR_SCENE_ENV_SOURCE_CUBEMAP) {
    VkrTexture *source = vkr_texture_system_get_by_handle(
        &rf->texture_system, environment->source_cubemap);
    if (!source || !source->handle ||
        source->description.type != VKR_TEXTURE_TYPE_CUBE_MAP) {
      goto failed;
    }
    environment->source_face_size = source->description.width;
    environment->source_mip_count = 1u;
  } else {
    goto failed;
  }

  char source_name_storage[128];
  char irradiance_name_storage[128];
  char prefilter_name_storage[128];
  snprintf(source_name_storage, sizeof(source_name_storage),
           "__ibl.scene.%p.source", (void *)scene);
  snprintf(irradiance_name_storage, sizeof(irradiance_name_storage),
           "__ibl.scene.%p.irradiance", (void *)scene);
  snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
           "__ibl.scene.%p.prefilter", (void *)scene);
  const String8 source_name = string8_create_from_cstr(
      (const uint8_t *)source_name_storage, string_length(source_name_storage));
  const String8 irradiance_name =
      string8_create_from_cstr((const uint8_t *)irradiance_name_storage,
                               string_length(irradiance_name_storage));
  const String8 prefilter_name =
      string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                               string_length(prefilter_name_storage));

  if ((environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT &&
       !vkr_world_resources_create_writable_cube_texture(
           rf, source_name, environment->source_face_size, true_v,
           VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
           &environment->source_cubemap)) ||
      !vkr_world_resources_create_writable_cube_texture(
          rf, irradiance_name, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, false_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          &environment->irradiance_cubemap) ||
      !vkr_world_resources_create_writable_cube_texture(
          rf, prefilter_name, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE, true_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          &environment->prefilter_cubemap)) {
    goto failed;
  }

  const bool8_t baked =
      environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT
          ? rf->asset_publisher.bake_hdr_environment(
                rf->asset_publisher.state, environment->delivery_equirect,
                environment->source_cubemap, environment->irradiance_cubemap,
                environment->prefilter_cubemap)
          : rf->asset_publisher.bake_ibl_cubemap(
                rf->asset_publisher.state, environment->source_cubemap,
                environment->irradiance_cubemap,
                environment->prefilter_cubemap);
  if (!baked) {
    goto failed;
  }
  (void)vkr_world_resources_release_texture(&rf->texture_system,
                                            &environment->delivery_equirect);
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_READY;
  vkr_world_resources_retain_first_scene_environment_as_default(rf, resources,
                                                                environment);
  return true_v;

failed:
  vkr_world_resources_fail_scene_environment(rf, scene);
  return false_v;
}

bool8_t vkr_world_resources_prepare_scene_environment(
    RendererFrontend *rf, VkrWorldResources *resources, VkrScene *scene) {
  if (!rf || !resources || !scene || !scene->environment.enabled ||
      !vkr_world_resources_has_retained_ibl_publisher(rf)) {
    if (rf && scene) {
      vkr_world_resources_fail_scene_environment(rf, scene);
    }
    return false_v;
  }
  return vkr_world_resources_prepare_published_environment(rf, resources,
                                                           scene);
}

void vkr_world_resources_bake_scene_ibl_if_pending(RendererFrontend *rf,
                                                   VkrWorldResources *resources,
                                                   VkrScene *scene) {
  (void)rf;
  (void)resources;
  (void)scene;
}

void vkr_world_resources_release_scene_reflection_probe_targets(
    RendererFrontend *rf, VkrScene *scene) {
  (void)rf;
  (void)scene;
}

vkr_internal void
vkr_world_resources_fail_reflection_probe(RendererFrontend *rf,
                                          VkrSceneReflectionProbe *probe) {
  if (!rf || !probe) {
    return;
  }

  vkr_world_resources_release_texture(&rf->texture_system,
                                      &probe->irradiance_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &probe->prefilter_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &probe->source_cubemap);
  probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_FAILED;
}

vkr_internal bool8_t vkr_world_resources_retain_environment_probe_maps(
    RendererFrontend *rf, VkrScene *scene, VkrSceneReflectionProbe *probe) {
  if (!rf || !scene || !probe ||
      scene->environment.bake_state != VKR_SCENE_ENV_BAKE_STATE_READY ||
      !vkr_world_resources_texture_is_valid(
          &rf->texture_system, scene->environment.irradiance_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP) ||
      !vkr_world_resources_texture_is_valid(
          &rf->texture_system, scene->environment.prefilter_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP)) {
    return false_v;
  }

  vkr_texture_system_add_ref_by_handle(&rf->texture_system,
                                       scene->environment.irradiance_cubemap);
  vkr_texture_system_add_ref_by_handle(&rf->texture_system,
                                       scene->environment.prefilter_cubemap);
  probe->irradiance_cubemap = scene->environment.irradiance_cubemap;
  probe->prefilter_cubemap = scene->environment.prefilter_cubemap;
  probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY;
  return true_v;
}

bool8_t vkr_world_resources_prepare_scene_reflection_probes(
    RendererFrontend *rf, VkrWorldResources *resources, VkrScene *scene) {
  if (!rf || !resources || !scene ||
      !vkr_world_resources_has_retained_ibl_publisher(rf)) {
    return false_v;
  }

  bool8_t all_prepared = true_v;
  for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
    VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
    if (!probe->enabled ||
        probe->bake_state != VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING) {
      continue;
    }
    if (probe->uses_scene_environment_source) {
      if (scene->environment.bake_state != VKR_SCENE_ENV_BAKE_STATE_READY) {
        if (scene->environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_FAILED) {
          vkr_world_resources_fail_reflection_probe(rf, probe);
          all_prepared = false_v;
        }
        continue;
      }
      if (!vkr_world_resources_retain_environment_probe_maps(rf, scene,
                                                             probe)) {
        vkr_world_resources_fail_reflection_probe(rf, probe);
        all_prepared = false_v;
      }
      continue;
    }

    char irradiance_name_storage[160];
    char prefilter_name_storage[160];
    snprintf(irradiance_name_storage, sizeof(irradiance_name_storage),
             "__ibl.scene.%p.probe.%u.irradiance", (void *)scene, i);
    snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
             "__ibl.scene.%p.probe.%u.prefilter", (void *)scene, i);
    String8 irradiance_name =
        string8_create_from_cstr((const uint8_t *)irradiance_name_storage,
                                 string_length(irradiance_name_storage));
    String8 prefilter_name =
        string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                                 string_length(prefilter_name_storage));
    if (!vkr_world_resources_create_writable_cube_texture(
            rf, irradiance_name, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE,
            false_v, VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
            &probe->irradiance_cubemap) ||
        !vkr_world_resources_create_writable_cube_texture(
            rf, prefilter_name, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE, true_v,
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
            &probe->prefilter_cubemap) ||
        !rf->asset_publisher.bake_ibl_cubemap(
            rf->asset_publisher.state, probe->source_cubemap,
            probe->irradiance_cubemap, probe->prefilter_cubemap)) {
      vkr_world_resources_fail_reflection_probe(rf, probe);
      all_prepared = false_v;
      continue;
    }
    probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY;
  }
  return all_prepared;
}

void vkr_world_resources_bake_scene_reflection_probes_if_pending(
    RendererFrontend *rf, VkrWorldResources *resources, VkrScene *scene) {
  (void)vkr_world_resources_prepare_scene_reflection_probes(rf, resources,
                                                            scene);
}

float32_t vkr_world_resources_probe_fragment_influence(Vec3 center,
                                                       Vec3 extents,
                                                       float32_t blend_distance,
                                                       Vec3 world_position) {
  float32_t dx = vkr_abs_f32(world_position.x - center.x);
  float32_t dy = vkr_abs_f32(world_position.y - center.y);
  float32_t dz = vkr_abs_f32(world_position.z - center.z);
  float32_t outside_x = vkr_max_f32(0.0f, dx - extents.x);
  float32_t outside_y = vkr_max_f32(0.0f, dy - extents.y);
  float32_t outside_z = vkr_max_f32(0.0f, dz - extents.z);
  float32_t outside_distance = vkr_sqrt_f32(
      outside_x * outside_x + outside_y * outside_y + outside_z * outside_z);

  if (outside_distance <= 1e-6f) {
    return 1.0f;
  }
  if (blend_distance <= 0.0f) {
    return 0.0f;
  }

  return vkr_max_f32(0.0f, 1.0f - outside_distance / blend_distance);
}

bool8_t vkr_world_resources_probe_intersects_sphere(Vec3 center, Vec3 extents,
                                                    float32_t blend_distance,
                                                    Vec3 sphere_center,
                                                    float32_t sphere_radius) {
  Vec3 influence_extents = vec3_add(
      extents, vec3_new(blend_distance, blend_distance, blend_distance));
  Vec3 delta = vec3_new(vkr_abs_f32(sphere_center.x - center.x),
                        vkr_abs_f32(sphere_center.y - center.y),
                        vkr_abs_f32(sphere_center.z - center.z));
  Vec3 outside = vec3_new(vkr_max_f32(0.0f, delta.x - influence_extents.x),
                          vkr_max_f32(0.0f, delta.y - influence_extents.y),
                          vkr_max_f32(0.0f, delta.z - influence_extents.z));
  float32_t radius = vkr_max_f32(sphere_radius, 0.0f);
  return vec3_length_squared(outside) <= radius * radius ? true_v : false_v;
}

vkr_internal VkrWorldIblProbeSlot vkr_world_resources_fallback_probe_slot(
    RendererFrontend *rf, VkrWorldResources *resources) {
  VkrWorldIblProbeSlot slot = {
      .irradiance_map = NULL,
      .prefilter_map = NULL,
      .center = {0},
      .extents = {0},
      .blend_distance = 0.0f,
      .weight = 1.0f,
      .intensity = 1.0f,
      .diffuse_intensity = 1.0f,
      .specular_intensity = 1.0f,
      .box_projection_enabled = false_v,
  };

  if (!rf || !resources) {
    return slot;
  }

  slot.irradiance_map = vkr_world_resources_resolve_backend_texture(
      &rf->texture_system, resources->ibl_active_irradiance_cubemap,
      VKR_TEXTURE_TYPE_CUBE_MAP);
  slot.prefilter_map = vkr_world_resources_resolve_backend_texture(
      &rf->texture_system, resources->ibl_active_prefilter_cubemap,
      VKR_TEXTURE_TYPE_CUBE_MAP);
  if (!slot.irradiance_map) {
    slot.irradiance_map = vkr_world_resources_resolve_backend_texture(
        &rf->texture_system, resources->ibl_fallback_irradiance_cubemap,
        VKR_TEXTURE_TYPE_CUBE_MAP);
  }
  if (!slot.prefilter_map) {
    slot.prefilter_map = vkr_world_resources_resolve_backend_texture(
        &rf->texture_system, resources->ibl_fallback_prefilter_cubemap,
        VKR_TEXTURE_TYPE_CUBE_MAP);
  }
  return slot;
}

void vkr_world_resources_select_probe_slots_for_position(
    RendererFrontend *rf, VkrWorldResources *resources, const VkrScene *scene,
    Vec3 world_position, VkrWorldIblProbeSlot out_slots[3]) {
  vkr_world_resources_select_probe_slots_for_bounds(
      rf, resources, scene, world_position, 0.0f, out_slots);
}

void vkr_world_resources_select_probe_slots_for_bounds(
    RendererFrontend *rf, VkrWorldResources *resources, const VkrScene *scene,
    Vec3 bounds_center, float32_t bounds_radius,
    VkrWorldIblProbeSlot out_slots[3]) {
  if (!out_slots) {
    return;
  }

  MemZero(out_slots, sizeof(VkrWorldIblProbeSlot) * 3u);
  if (!rf || !resources) {
    out_slots[2].weight = 1.0f;
    return;
  }

  if (!resources->ibl_default_ready) {
    out_slots[2].weight = 1.0f;
    return;
  }

  VkrWorldIblProbeSlot fallback =
      vkr_world_resources_fallback_probe_slot(rf, resources);
  out_slots[0] = fallback;
  out_slots[1] = fallback;
  out_slots[2] = fallback;
  out_slots[0].weight = 0.0f;
  out_slots[1].weight = 0.0f;
  out_slots[2].weight = 1.0f;

  uint32_t best_probe_index[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
  float32_t best_probe_score[2] = {-VKR_FLOAT_MAX, -VKR_FLOAT_MAX};
  if (scene) {
    for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
      const VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
      if (!probe->enabled ||
          probe->bake_state != VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY) {
        continue;
      }

      VkrTextureOpaqueHandle irradiance =
          vkr_world_resources_resolve_backend_texture(
              &rf->texture_system, probe->irradiance_cubemap,
              VKR_TEXTURE_TYPE_CUBE_MAP);
      VkrTextureOpaqueHandle prefilter =
          vkr_world_resources_resolve_backend_texture(
              &rf->texture_system, probe->prefilter_cubemap,
              VKR_TEXTURE_TYPE_CUBE_MAP);
      if (!irradiance || !prefilter) {
        continue;
      }

      if (!vkr_world_resources_probe_intersects_sphere(
              probe->center, probe->extents, probe->blend_distance,
              bounds_center, bounds_radius)) {
        continue;
      }

      float32_t center_distance =
          vec3_length(vec3_sub(bounds_center, probe->center));
      float32_t score = -center_distance;
      if (score > best_probe_score[0]) {
        best_probe_score[1] = best_probe_score[0];
        best_probe_index[1] = best_probe_index[0];
        best_probe_score[0] = score;
        best_probe_index[0] = i;
      } else if (score > best_probe_score[1]) {
        best_probe_score[1] = score;
        best_probe_index[1] = i;
      }
    }
  }

  for (uint32_t slot_index = 0; slot_index < 2u; ++slot_index) {
    if (best_probe_index[slot_index] == 0xFFFFFFFFu) {
      continue;
    }
    const VkrSceneReflectionProbe *probe =
        &scene->reflection_probes[best_probe_index[slot_index]];
    out_slots[slot_index] = (VkrWorldIblProbeSlot){
        .irradiance_map = vkr_world_resources_resolve_backend_texture(
            &rf->texture_system, probe->irradiance_cubemap,
            VKR_TEXTURE_TYPE_CUBE_MAP),
        .prefilter_map = vkr_world_resources_resolve_backend_texture(
            &rf->texture_system, probe->prefilter_cubemap,
            VKR_TEXTURE_TYPE_CUBE_MAP),
        .center = probe->center,
        .extents = probe->extents,
        .blend_distance = probe->blend_distance,
        .weight = 1.0f,
        .intensity = probe->intensity,
        .diffuse_intensity = probe->diffuse_intensity,
        .specular_intensity = probe->specular_intensity,
        .box_projection_enabled = true_v,
    };
  }
}

void vkr_world_resources_set_active_ibl_from_scene_or_default(
    RendererFrontend *rf, VkrWorldResources *resources, const VkrScene *scene) {
  if (!rf || !resources) {
    return;
  }

  if (!resources->ibl_default_ready) {
    resources->ibl_active_enabled = false_v;
    resources->ibl_active_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
    resources->ibl_active_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
    return;
  }

  bool8_t use_scene_ibl =
      scene && scene->environment.enabled &&
      scene->environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_READY &&
      vkr_world_resources_texture_is_valid(
          &rf->texture_system, scene->environment.irradiance_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP) &&
      vkr_world_resources_texture_is_valid(&rf->texture_system,
                                           scene->environment.prefilter_cubemap,
                                           VKR_TEXTURE_TYPE_CUBE_MAP);

  if (use_scene_ibl) {
    resources->ibl_active_irradiance_cubemap =
        scene->environment.irradiance_cubemap;
    resources->ibl_active_prefilter_cubemap =
        scene->environment.prefilter_cubemap;
    resources->ibl_active_enabled = true_v;
    resources->ibl_active_intensity = scene->environment.intensity;
    resources->ibl_active_diffuse_intensity =
        scene->environment.diffuse_intensity;
    resources->ibl_active_specular_intensity =
        scene->environment.specular_intensity;
    return;
  }

  resources->ibl_active_irradiance_cubemap =
      resources->ibl_fallback_irradiance_cubemap;
  resources->ibl_active_prefilter_cubemap =
      resources->ibl_fallback_prefilter_cubemap;
  resources->ibl_active_enabled = true_v;
  resources->ibl_active_intensity = 1.0f;
  resources->ibl_active_diffuse_intensity = 1.0f;
  resources->ibl_active_specular_intensity = 1.0f;
}

void vkr_world_resources_shutdown(RendererFrontend *rf,
                                  VkrWorldResources *resources) {
  if (!rf || !resources) {
    return;
  }
  for (uint64_t i = 0; i < resources->text_slots.length; ++i) {
    VkrWorldTextSlot *slot = &resources->text_slots.data[i];
    if (slot->active) {
      vkr_text_3d_destroy(&slot->text);
    }
  }
  array_destroy_VkrWorldTextSlot(&resources->text_slots);

  vkr_world_resources_release_texture(
      &rf->texture_system, &resources->ibl_fallback_prefilter_cubemap);
  vkr_world_resources_release_texture(
      &rf->texture_system, &resources->ibl_fallback_irradiance_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &resources->ibl_fallback_source_cubemap);
  MemZero(resources, sizeof(*resources));
}

bool8_t vkr_world_resources_text_create(RendererFrontend *rf,
                                        VkrWorldResources *resources,
                                        const VkrWorldTextCreateData *payload) {
  if (!rf || !resources || !payload) {
    return false_v;
  }

  VkrWorldTextSlot *slot = NULL;
  if (!vkr_world_resources_ensure_text_slot(resources, payload->text_id,
                                            &slot)) {
    return false_v;
  }

  if (slot->active) {
    vkr_text_3d_destroy(&slot->text);
    slot->active = false_v;
  }

  VkrText3DConfig config =
      payload->config ? *payload->config : VKR_TEXT_3D_CONFIG_DEFAULT;
  config.text = payload->content;
  VkrRendererError text_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_text_3d_create(&slot->text, &rf->font_system, &rf->allocator,
                          &config, &text_err)) {
    String8 err = vkr_renderer_get_error_string(text_err);
    log_error("Failed to create world text: %s", string8_cstr(&err));
    return false_v;
  }

  vkr_text_3d_set_transform(&slot->text, payload->transform);
  slot->active = true_v;
  return true_v;
}

bool8_t vkr_world_resources_text_update(RendererFrontend *rf,
                                        VkrWorldResources *resources,
                                        uint32_t text_id, String8 content) {
  (void)rf;
  if (!resources) {
    return false_v;
  }

  VkrWorldTextSlot *slot =
      vkr_world_resources_get_text_slot(resources, text_id);
  if (!slot) {
    log_warn("World text id %u not found for update", text_id);
    return false_v;
  }

  vkr_text_3d_set_text(&slot->text, content);
  return true_v;
}

bool8_t vkr_world_resources_text_set_transform(RendererFrontend *rf,
                                               VkrWorldResources *resources,
                                               uint32_t text_id,
                                               const VkrTransform *transform) {
  (void)rf;
  if (!resources || !transform) {
    return false_v;
  }

  VkrWorldTextSlot *slot =
      vkr_world_resources_get_text_slot(resources, text_id);
  if (!slot) {
    log_warn("World text id %u not found for transform", text_id);
    return false_v;
  }

  vkr_text_3d_set_transform(&slot->text, *transform);
  return true_v;
}

bool8_t vkr_world_resources_text_destroy(RendererFrontend *rf,
                                         VkrWorldResources *resources,
                                         uint32_t text_id) {
  (void)rf;
  if (!resources) {
    return false_v;
  }

  VkrWorldTextSlot *slot =
      vkr_world_resources_get_text_slot(resources, text_id);
  if (!slot) {
    log_warn("World text id %u not found for destroy", text_id);
    return false_v;
  }

  vkr_text_3d_destroy(&slot->text);
  slot->active = false_v;
  return true_v;
}

uint32_t vkr_world_resources_prepare_text_draws(RendererFrontend *rf,
                                                VkrWorldResources *resources,
                                                VkrPreparedTextDraw *out_draws,
                                                uint32_t capacity) {
  uint32_t count = 0;
  for (uint64_t i = 0; i < resources->text_slots.length; ++i) {
    VkrWorldTextSlot *slot = &resources->text_slots.data[i];
    VkrText3D *text = &slot->text;
    if (!slot->active || !vkr_text_3d_prepare_geometry(text) ||
        text->index_count == 0)
      continue;
    if (count == capacity) {
      log_error("World text packet capacity exceeded (%u)", capacity);
      break;
    }
    VkrFont *font =
        vkr_font_system_get_by_handle(text->font_system, text->font);
    if (!font)
      font = vkr_font_system_get_default_mtsdf_font(text->font_system);
    if (!font || font->atlas.id == 0 ||
        font->atlas.generation == VKR_INVALID_ID)
      continue;
    float32_t screen_px_range = 0.0f;
    uint32_t font_mode = 0;
    if (font->type == VKR_FONT_TYPE_MTSDF && font->em_size > 0.0f) {
      const float32_t render_size =
          text->font_size > 0.0f ? text->font_size : (float32_t)font->size;
      font_mode = 1;
      screen_px_range = Clamp(
          font->sdf_distance_range * (render_size / font->em_size), 1.0f, 4.0f);
    }
    Mat4 model = vkr_transform_get_world(&text->transform);
    if (text->texture_width > 0 && text->texture_height > 0) {
      model = mat4_mul(
          model, mat4_scale(vec3_new(text->world_width / text->texture_width,
                                     text->world_height / text->texture_height,
                                     1.0f)));
    }
    out_draws[count++] = (VkrPreparedTextDraw){
        .vertices = text->vertices,
        .vertex_count = text->vertex_count,
        .indices = text->indices,
        .index_count = text->index_count,
        .max_index = text->vertex_count - 1u,
        .atlas = font->atlas,
        .model = model,
        .screen_px_range = screen_px_range,
        .font_mode = font_mode,
        .object_id =
            vkr_picking_encode_id(VKR_PICKING_ID_KIND_WORLD_TEXT, (uint32_t)i),
        .revision = text->geometry_revision,
    };
  }
  return count;
}
