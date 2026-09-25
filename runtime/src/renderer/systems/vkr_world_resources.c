/**
 * @file vkr_world_resources.c
 * @brief Scene environment and probe bake preparation, and 3D text slots.
 */

#include "renderer/systems/vkr_world_resources.h"

#include <math.h>
#include <stdio.h>

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "math/vkr_transform.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_render_assets.h"
#include "renderer/systems/vkr_scene_system.h"
#include "vkr_frame_input.h"
#include "vkr_ibl_math.h"

#define VKR_WORLD_RESOURCES_MAX_TEXTS 16

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
vkr_world_resources_has_retained_ibl_publisher(const VkrRenderAssets *assets) {
  return assets && assets->asset_publisher &&
         assets->asset_publisher->publish_writable_texture &&
         assets->asset_publisher->bake_ibl_cubemap &&
         assets->asset_publisher->ibl_sh_slot;
}

vkr_internal bool8_t vkr_world_resources_create_writable_texture(
    VkrRenderAssets *assets, String8 name, VkrTextureType type, uint32_t width,
    uint32_t height, bool8_t with_mips, VkrTextureFormat format,
    VkrTextureHandle *out_handle) {
  if (!assets || !name.str || !out_handle || width == 0 || height == 0) {
    return false_v;
  }

  VkrTextureDescription desc = {
      .width = width,
      .height = height,
      .channels = 4,
      .type = type,
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
  if (!vkr_texture_system_create_writable(&assets->texture_system, name, &desc,
                                          out_handle, &texture_err)) {
    String8 err = vkr_renderer_get_error_string(texture_err);
    log_warn("World resources: failed to create writable texture '%.*s': %s",
             (int)name.length, name.str, string8_cstr(&err));
    return false_v;
  }

  return true_v;
}

bool8_t vkr_world_resources_init(VkrRenderAssets *assets,
                                 VkrWorldResources *resources) {
  if (!assets || !resources) {
    return false_v;
  }
  MemZero(resources, sizeof(*resources));
  resources->text_slots = array_create_VkrWorldTextSlot(
      &assets->allocator, VKR_WORLD_RESOURCES_MAX_TEXTS);
  if (!resources->text_slots.data) {
    return false_v;
  }
  MemZero(resources->text_slots.data,
          sizeof(VkrWorldTextSlot) * (uint64_t)resources->text_slots.length);
  resources->initialized = true_v;
  return true_v;
}
vkr_internal void
vkr_world_resources_fail_scene_environment(VkrRenderAssets *assets,
                                           VkrScene *scene) {
  if (!assets || !scene) {
    return;
  }

  VkrSceneEnvironment *environment = &scene->environment;
  vkr_world_resources_release_texture(&assets->texture_system,
                                      &environment->prefilter_cubemap);
  vkr_world_resources_release_texture(&assets->texture_system,
                                      &environment->source_cubemap);
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
}

/* One texel per face represents a uniform environment exactly: the visible
   sky, GGX prefilter and SH projection all read the same radiance. The name
   carries the half-float radiance, so a reused name has identical contents. */
vkr_internal bool8_t vkr_world_resources_create_constant_source(
    VkrRenderAssets *assets, const VkrScene *scene,
    VkrTextureHandle *out_handle) {
  const Vec3 radiance = scene->environment.constant_radiance;
  /* Environment sources carry no sun disc; their alpha is unused. */
  const uint16_t texel[4] = {
      vkr_float32_to_float16(radiance.x),
      vkr_float32_to_float16(radiance.y),
      vkr_float32_to_float16(radiance.z),
      0u,
  };
  uint16_t faces[6][4];
  VkrTextureUploadRegion regions[6];
  _Static_assert(sizeof(faces) == VKR_WORLD_RESOURCES_CONSTANT_CUBE_BYTES,
                 "Constant environment upload size drift");
  for (uint32_t face = 0; face < ArrayCount(regions); ++face) {
    MemCopy(faces[face], texel, sizeof(texel));
    regions[face] = (VkrTextureUploadRegion){
        .mip_level = 0u,
        .array_layer = face,
        .width = 1u,
        .height = 1u,
        .depth = 1u,
        .byte_offset = (uint64_t)face * sizeof(texel),
        .byte_size = sizeof(texel),
    };
  }

  const VkrTexturePreparedLoad prepared = {
      .description =
          {
              .width = 1u,
              .height = 1u,
              .channels = 4u,
              .mip_levels = 1u,
              .array_layers = 6u,
              .type = VKR_TEXTURE_TYPE_CUBE_MAP,
              .format = VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
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
              .generation = VKR_INVALID_ID,
          },
      .upload_data = (uint8_t *)faces,
      .upload_data_size = sizeof(faces),
      .upload_regions = regions,
      .upload_region_count = ArrayCount(regions),
      .upload_mip_levels = 1u,
      .upload_array_layers = 6u,
      .upload_is_compressed = false_v,
  };

  char name_storage[160];
  snprintf(name_storage, sizeof(name_storage),
           "__ibl.scene.%p.constant.%04x%04x%04x", (const void *)scene,
           texel[0], texel[1], texel[2]);
  const String8 name = string8_create_from_cstr((const uint8_t *)name_storage,
                                                string_length(name_storage));
  VkrRendererError texture_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_finalize_prepared_load(
          &assets->texture_system, name, &prepared, out_handle, &texture_err)) {
    String8 err = vkr_renderer_get_error_string(texture_err);
    log_warn("World resources: failed to upload constant environment: %s",
             string8_cstr(&err));
    return false_v;
  }
  vkr_texture_system_add_ref_by_handle(&assets->texture_system, *out_handle);
  return true_v;
}

vkr_internal bool8_t vkr_world_resources_prepare_published_environment(
    VkrRenderAssets *assets, VkrScene *scene) {
  VkrSceneEnvironment *environment = &scene->environment;
  if (!vkr_world_resources_create_constant_source(
          assets, scene, &environment->source_cubemap)) {
    goto failed;
  }
  environment->source_face_size = 1u;
  environment->source_mip_count = 1u;

  char prefilter_name_storage[128];
  snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
           "__ibl.scene.%p.prefilter", (void *)scene);
  const String8 prefilter_name =
      string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                               string_length(prefilter_name_storage));
  if (!vkr_world_resources_create_writable_texture(
          assets, prefilter_name, VKR_TEXTURE_TYPE_CUBE_MAP,
          VKR_IBL_PREFILTER_SIZE, VKR_IBL_PREFILTER_SIZE, true_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          &environment->prefilter_cubemap) ||
      !assets->asset_publisher->bake_ibl_cubemap(
          assets->asset_publisher->state, environment->source_cubemap,
          environment->prefilter_cubemap, environment->sh_deringing)) {
    goto failed;
  }
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_READY;
  return true_v;

failed:
  vkr_world_resources_fail_scene_environment(assets, scene);
  return false_v;
}

bool8_t vkr_world_resources_prepare_scene_environment(
    VkrRenderAssets *assets, VkrWorldResources *resources, VkrScene *scene) {
  if (!assets || !resources || !scene || !scene->environment.enabled ||
      scene->environment.source_kind != VKR_SCENE_ENV_SOURCE_CONSTANT ||
      !vkr_world_resources_has_retained_ibl_publisher(assets)) {
    if (assets && scene) {
      vkr_world_resources_fail_scene_environment(assets, scene);
    }
    return false_v;
  }
  return vkr_world_resources_prepare_published_environment(assets, scene);
}

vkr_internal bool8_t
vkr_world_resources_has_atmosphere_publisher(const VkrRenderAssets *assets) {
  return assets && assets->asset_publisher &&
         assets->asset_publisher->bake_atmosphere &&
         assets->asset_publisher->atmosphere_bake_status;
}

vkr_internal bool8_t vkr_world_resources_clear_atmosphere_candidate(
    VkrRenderAssets *assets, VkrSceneAtmosphere *atmosphere) {
  if (!assets || !atmosphere)
    return false_v;
  VkrTextureHandle *handles[] = {
      &atmosphere->candidate_prefilter_cubemap,
      &atmosphere->candidate_source_cubemap,
      &atmosphere->candidate_transmittance,
      &atmosphere->candidate_multiple_scattering,
  };
  bool8_t released = true_v;
  for (uint32_t i = 0; i < ArrayCount(handles); ++i) {
    if (!vkr_world_resources_release_texture(&assets->texture_system,
                                             handles[i]))
      released = false_v;
  }
  if (!released) {
    log_warn("Atmosphere candidate release remains pending; preserving owned "
             "handles for retry");
    return false_v;
  }
  atmosphere->candidate_settings = vkr_atmosphere_settings_defaults();
  atmosphere->candidate_settings.enabled = false_v;
  atmosphere->candidate_clouds = vkr_cloud_settings_defaults();
  return true_v;
}

/* Detaches the published tuple and keeps the authored sky-light controls. */
vkr_internal void
vkr_world_resources_detach_environment_tuple(VkrSceneEnvironment *environment) {
  environment->source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  environment->prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  environment->atmosphere_transmittance = VKR_TEXTURE_HANDLE_INVALID;
  environment->atmosphere_multiple_scattering = VKR_TEXTURE_HANDLE_INVALID;
  environment->source_face_size = 0u;
  environment->source_mip_count = 0u;
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_NONE;
}

vkr_internal bool8_t vkr_world_resources_release_environment_tuple(
    VkrRenderAssets *assets, VkrSceneEnvironment *environment) {
  if (!assets || !environment)
    return false_v;
  VkrTextureHandle *handles[] = {
      &environment->prefilter_cubemap,
      &environment->source_cubemap,
      &environment->atmosphere_transmittance,
      &environment->atmosphere_multiple_scattering,
  };
  bool8_t released = true_v;
  for (uint32_t i = 0; i < ArrayCount(handles); ++i) {
    if (!vkr_world_resources_release_texture(&assets->texture_system,
                                             handles[i]))
      released = false_v;
  }
  if (!released) {
    log_warn("Retired atmosphere environment release remains pending; "
             "preserving owned handles for retry");
    return false_v;
  }
  vkr_world_resources_detach_environment_tuple(environment);
  return true_v;
}

vkr_internal bool8_t vkr_world_resources_drain_retired_atmosphere_environment(
    VkrRenderAssets *assets, VkrSceneAtmosphere *atmosphere) {
  if (!assets || !atmosphere)
    return false_v;
  const VkrSceneEnvironment *retired = &atmosphere->retired_environment;
  if (retired->source_cubemap.id == 0u && retired->prefilter_cubemap.id == 0u &&
      retired->atmosphere_transmittance.id == 0u &&
      retired->atmosphere_multiple_scattering.id == 0u)
    return true_v;
  return vkr_world_resources_release_environment_tuple(
      assets, &atmosphere->retired_environment);
}

vkr_internal bool8_t vkr_world_resources_disable_scene_atmosphere(
    VkrRenderAssets *assets, VkrScene *scene) {
  VkrSceneAtmosphere *atmosphere = &scene->atmosphere;
  if (!vkr_world_resources_clear_atmosphere_candidate(assets, atmosphere) ||
      !vkr_world_resources_drain_retired_atmosphere_environment(assets,
                                                                atmosphere))
    return false_v;
  atmosphere->candidate_revision = 0u;
  atmosphere->bake_state = VKR_SCENE_ATMOSPHERE_BAKE_STATE_NONE;
  if (!atmosphere->active_revision)
    return true_v;

  atmosphere->retired_environment = scene->environment;
  vkr_world_resources_detach_environment_tuple(&scene->environment);
  atmosphere->active_settings = vkr_atmosphere_settings_defaults();
  atmosphere->active_settings.enabled = false_v;
  atmosphere->active_clouds = vkr_cloud_settings_defaults();
  atmosphere->active_revision = 0u;
  return vkr_world_resources_drain_retired_atmosphere_environment(assets,
                                                                  atmosphere);
}

/* One writable texture of an atmosphere generation. */
typedef struct VkrAtmosphereCandidateTexture {
  const char *suffix;
  VkrTextureType type;
  uint32_t width;
  uint32_t height;
  bool8_t with_mips;
  VkrTextureHandle *handle;
} VkrAtmosphereCandidateTexture;

bool8_t vkr_world_resources_prepare_scene_atmosphere(
    VkrRenderAssets *assets, VkrWorldResources *resources, VkrScene *scene) {
  (void)resources;
  if (!assets || !scene)
    return false_v;

  VkrSceneAtmosphere *atmosphere = &scene->atmosphere;
  if (!atmosphere->requested_revision)
    return true_v;
  if (!vkr_world_resources_drain_retired_atmosphere_environment(assets,
                                                                atmosphere))
    return false_v;
  if (!atmosphere->requested_settings.enabled) {
    if (!atmosphere->active_revision && !atmosphere->candidate_revision &&
        atmosphere->bake_state == VKR_SCENE_ATMOSPHERE_BAKE_STATE_NONE)
      return true_v;
    return vkr_world_resources_disable_scene_atmosphere(assets, scene);
  }
  if (atmosphere->active_revision == atmosphere->requested_revision)
    return true_v;
  /* A candidate in flight publishes before a newer request bakes. Cancelling
     it would starve publication while a directional light keeps moving the
     sun; the latest request starts once this one publishes. */
  if (atmosphere->bake_state == VKR_SCENE_ATMOSPHERE_BAKE_STATE_PENDING &&
      atmosphere->candidate_revision &&
      atmosphere->candidate_source_cubemap.id != 0u) {
    return true_v;
  }
  if (atmosphere->candidate_revision &&
      atmosphere->candidate_revision != atmosphere->requested_revision) {
    if (!vkr_world_resources_clear_atmosphere_candidate(assets, atmosphere))
      return false_v;
    atmosphere->candidate_revision = 0u;
    atmosphere->bake_state = VKR_SCENE_ATMOSPHERE_BAKE_STATE_NONE;
  }
  if (atmosphere->bake_state == VKR_SCENE_ATMOSPHERE_BAKE_STATE_FAILED &&
      atmosphere->candidate_revision == atmosphere->requested_revision) {
    if ((atmosphere->candidate_source_cubemap.id != 0u ||
         atmosphere->candidate_prefilter_cubemap.id != 0u ||
         atmosphere->candidate_transmittance.id != 0u ||
         atmosphere->candidate_multiple_scattering.id != 0u) &&
        !vkr_world_resources_clear_atmosphere_candidate(assets, atmosphere))
      return false_v;
    return false_v;
  }
  if (!vkr_atmosphere_settings_valid(&atmosphere->requested_settings) ||
      !vkr_cloud_settings_valid(&atmosphere->requested_clouds) ||
      !isfinite(atmosphere->requested_sh_deringing) ||
      atmosphere->requested_sh_deringing < 0.0f ||
      !vkr_world_resources_has_atmosphere_publisher(assets)) {
    atmosphere->bake_state = VKR_SCENE_ATMOSPHERE_BAKE_STATE_FAILED;
    atmosphere->candidate_revision = atmosphere->requested_revision;
    return false_v;
  }

  /* Each generation owns its source, prefilter and lookup textures, so the
     published sky keeps reading its own lookups while a candidate bakes. */
  const VkrAtmosphereCandidateTexture textures[] = {
      {"source", VKR_TEXTURE_TYPE_CUBE_MAP, VKR_ATMOSPHERE_SOURCE_SIZE,
       VKR_ATMOSPHERE_SOURCE_SIZE, true_v,
       &atmosphere->candidate_source_cubemap},
      {"prefilter", VKR_TEXTURE_TYPE_CUBE_MAP, VKR_IBL_PREFILTER_SIZE,
       VKR_IBL_PREFILTER_SIZE, true_v,
       &atmosphere->candidate_prefilter_cubemap},
      {"transmittance", VKR_TEXTURE_TYPE_2D, VKR_ATMOSPHERE_TRANSMITTANCE_WIDTH,
       VKR_ATMOSPHERE_TRANSMITTANCE_HEIGHT, false_v,
       &atmosphere->candidate_transmittance},
      {"multiple_scattering", VKR_TEXTURE_TYPE_2D,
       VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE,
       VKR_ATMOSPHERE_MULTIPLE_SCATTERING_SIZE, false_v,
       &atmosphere->candidate_multiple_scattering},
  };
  for (uint32_t i = 0; i < ArrayCount(textures); ++i) {
    char name_storage[160];
    snprintf(name_storage, sizeof(name_storage),
             "__atmosphere.scene.%p.%llu.%s", (void *)scene,
             (unsigned long long)atmosphere->requested_revision,
             textures[i].suffix);
    const String8 name = string8_create_from_cstr((const uint8_t *)name_storage,
                                                  string_length(name_storage));
    if (!vkr_world_resources_create_writable_texture(
            assets, name, textures[i].type, textures[i].width,
            textures[i].height, textures[i].with_mips,
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, textures[i].handle)) {
      (void)vkr_world_resources_clear_atmosphere_candidate(assets, atmosphere);
      atmosphere->candidate_revision = atmosphere->requested_revision;
      atmosphere->bake_state = VKR_SCENE_ATMOSPHERE_BAKE_STATE_FAILED;
      return false_v;
    }
  }

  const VkrAtmosphereGpuParams params =
      vkr_atmosphere_prepare(&atmosphere->requested_settings);
  if (!assets->asset_publisher->bake_atmosphere(
          assets->asset_publisher->state, &params,
          atmosphere->candidate_source_cubemap,
          atmosphere->candidate_prefilter_cubemap,
          atmosphere->candidate_transmittance,
          atmosphere->candidate_multiple_scattering,
          atmosphere->requested_sh_deringing)) {
    (void)vkr_world_resources_clear_atmosphere_candidate(assets, atmosphere);
    atmosphere->candidate_revision = atmosphere->requested_revision;
    atmosphere->bake_state = VKR_SCENE_ATMOSPHERE_BAKE_STATE_FAILED;
    return false_v;
  }
  atmosphere->candidate_settings = atmosphere->requested_settings;
  atmosphere->candidate_clouds = atmosphere->requested_clouds;
  atmosphere->candidate_revision = atmosphere->requested_revision;
  atmosphere->bake_state = VKR_SCENE_ATMOSPHERE_BAKE_STATE_PENDING;
  return true_v;
}

void vkr_world_resources_poll_scene_atmosphere(VkrRenderAssets *assets,
                                               VkrScene *scene) {
  if (!assets || !scene)
    return;
  VkrSceneAtmosphere *atmosphere = &scene->atmosphere;
  if (atmosphere->bake_state != VKR_SCENE_ATMOSPHERE_BAKE_STATE_PENDING ||
      !atmosphere->candidate_revision || !assets->asset_publisher ||
      !assets->asset_publisher->atmosphere_bake_status)
    return;

  const VkrAtmosphereBakeStatus status =
      assets->asset_publisher->atmosphere_bake_status(
          assets->asset_publisher->state, atmosphere->candidate_source_cubemap);
  if (status == VKR_ATMOSPHERE_BAKE_PENDING)
    return;
  if (status != VKR_ATMOSPHERE_BAKE_READY) {
    if (vkr_world_resources_clear_atmosphere_candidate(assets, atmosphere))
      atmosphere->bake_state = VKR_SCENE_ATMOSPHERE_BAKE_STATE_FAILED;
    return;
  }

  if (!vkr_world_resources_drain_retired_atmosphere_environment(assets,
                                                                atmosphere))
    return;

  /* Only the tuple changes; authored sky-light controls apply to it. */
  VkrSceneEnvironment previous = scene->environment;
  scene->environment.source_kind = VKR_SCENE_ENV_SOURCE_ATMOSPHERE;
  scene->environment.source_cubemap = atmosphere->candidate_source_cubemap;
  scene->environment.prefilter_cubemap =
      atmosphere->candidate_prefilter_cubemap;
  scene->environment.atmosphere_transmittance =
      atmosphere->candidate_transmittance;
  scene->environment.atmosphere_multiple_scattering =
      atmosphere->candidate_multiple_scattering;
  scene->environment.source_face_size = VKR_ATMOSPHERE_SOURCE_SIZE;
  scene->environment.source_mip_count = VKR_IBL_PREFILTER_MIP_COUNT;
  scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_READY;
  atmosphere->candidate_source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  atmosphere->candidate_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  atmosphere->candidate_transmittance = VKR_TEXTURE_HANDLE_INVALID;
  atmosphere->candidate_multiple_scattering = VKR_TEXTURE_HANDLE_INVALID;
  atmosphere->active_settings = atmosphere->candidate_settings;
  atmosphere->active_clouds = atmosphere->candidate_clouds;
  atmosphere->active_revision = atmosphere->candidate_revision;
  atmosphere->candidate_revision = 0u;
  atmosphere->bake_state = VKR_SCENE_ATMOSPHERE_BAKE_STATE_READY;
  atmosphere->retired_environment = previous;
  (void)vkr_world_resources_drain_retired_atmosphere_environment(assets,
                                                                 atmosphere);
}

void vkr_world_resources_bake_scene_ibl_if_pending(VkrRenderAssets *assets,
                                                   VkrWorldResources *resources,
                                                   VkrScene *scene) {
  (void)resources;
  (void)scene;
}
vkr_internal void
vkr_world_resources_fail_reflection_probe(VkrRenderAssets *assets,
                                          VkrSceneReflectionProbe *probe) {
  if (!assets || !probe) {
    return;
  }

  vkr_world_resources_release_texture(&assets->texture_system,
                                      &probe->prefilter_cubemap);
  vkr_world_resources_release_texture(&assets->texture_system,
                                      &probe->source_cubemap);
  probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_FAILED;
}

vkr_internal bool8_t vkr_world_resources_retain_environment_probe_maps(
    VkrRenderAssets *assets, VkrScene *scene, VkrSceneReflectionProbe *probe) {
  if (!assets || !scene || !probe ||
      scene->environment.bake_state != VKR_SCENE_ENV_BAKE_STATE_READY ||
      !vkr_world_resources_texture_is_valid(
          &assets->texture_system, scene->environment.prefilter_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP)) {
    return false_v;
  }

  vkr_texture_system_add_ref_by_handle(&assets->texture_system,
                                       scene->environment.prefilter_cubemap);
  probe->prefilter_cubemap = scene->environment.prefilter_cubemap;
  probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY;
  return true_v;
}

bool8_t vkr_world_resources_prepare_scene_reflection_probes(
    VkrRenderAssets *assets, VkrWorldResources *resources, VkrScene *scene) {
  if (!assets || !resources || !scene ||
      !vkr_world_resources_has_retained_ibl_publisher(assets)) {
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
          vkr_world_resources_fail_reflection_probe(assets, probe);
          all_prepared = false_v;
        }
        continue;
      }
      if (!vkr_world_resources_retain_environment_probe_maps(assets, scene,
                                                             probe)) {
        vkr_world_resources_fail_reflection_probe(assets, probe);
        all_prepared = false_v;
      }
      continue;
    }

    char prefilter_name_storage[160];
    snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
             "__ibl.scene.%p.probe.%u.prefilter", (void *)scene, i);
    String8 prefilter_name =
        string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                                 string_length(prefilter_name_storage));
    if (!vkr_world_resources_create_writable_texture(
            assets, prefilter_name, VKR_TEXTURE_TYPE_CUBE_MAP,
            VKR_IBL_PREFILTER_SIZE, VKR_IBL_PREFILTER_SIZE, true_v,
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
            &probe->prefilter_cubemap) ||
        !assets->asset_publisher->bake_ibl_cubemap(
            assets->asset_publisher->state, probe->source_cubemap,
            probe->prefilter_cubemap, probe->sh_deringing)) {
      vkr_world_resources_fail_reflection_probe(assets, probe);
      all_prepared = false_v;
      continue;
    }
    probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY;
  }
  return all_prepared;
}

void vkr_world_resources_bake_scene_reflection_probes_if_pending(
    VkrRenderAssets *assets, VkrWorldResources *resources, VkrScene *scene) {
  (void)vkr_world_resources_prepare_scene_reflection_probes(assets, resources,
                                                            scene);
}
void vkr_world_resources_shutdown(VkrRenderAssets *assets,
                                  VkrWorldResources *resources) {
  if (!assets || !resources) {
    return;
  }
  for (uint64_t i = 0; i < resources->text_slots.length; ++i) {
    VkrWorldTextSlot *slot = &resources->text_slots.data[i];
    if (slot->active) {
      vkr_text_3d_destroy(&slot->text);
    }
  }
  array_destroy_VkrWorldTextSlot(&resources->text_slots);
  MemZero(resources, sizeof(*resources));
}

bool8_t vkr_world_resources_text_create(VkrRenderAssets *assets,
                                        VkrWorldResources *resources,
                                        const VkrWorldTextCreateData *payload) {
  if (!assets || !resources || !payload) {
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
  if (!vkr_text_3d_create(&slot->text, &assets->font_system, &assets->allocator,
                          &config, &text_err)) {
    String8 err = vkr_renderer_get_error_string(text_err);
    log_error("Failed to create world text: %s", string8_cstr(&err));
    return false_v;
  }

  vkr_text_3d_set_transform(&slot->text, payload->transform);
  slot->active = true_v;
  return true_v;
}

bool8_t vkr_world_resources_text_update(VkrWorldResources *resources,
                                        uint32_t text_id, String8 content) {
  if (!resources) {
    return false_v;
  }

  VkrWorldTextSlot *slot =
      vkr_world_resources_get_text_slot(resources, text_id);
  if (!slot) {
    log_warn("World text id %u not found for update", text_id);
    return false_v;
  }

  return vkr_text_3d_set_text(&slot->text, content);
}

bool8_t vkr_world_resources_text_set_transform(VkrWorldResources *resources,
                                               uint32_t text_id,
                                               const VkrTransform *transform) {
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

bool8_t vkr_world_resources_text_destroy(VkrWorldResources *resources,
                                         uint32_t text_id) {
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

bool8_t vkr_world_resources_prepare_text_draws(VkrWorldResources *resources,
                                               VkrAllocator *scratch,
                                               VkrPreparedTextDraw **out_draws,
                                               uint32_t *out_count) {
  *out_draws = NULL;
  *out_count = 0u;
  uint32_t capacity = 0u;
  for (uint64_t i = 0u; i < resources->text_slots.length; ++i)
    capacity += resources->text_slots.data[i].active ? 1u : 0u;
  if (capacity == 0u)
    return true_v;
  VkrPreparedTextDraw *draws =
      vkr_allocator_alloc(scratch, (uint64_t)capacity * sizeof(*draws),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!draws)
    return false_v;
  uint32_t count = 0u;
  for (uint64_t i = 0; i < resources->text_slots.length; ++i) {
    VkrWorldTextSlot *slot = &resources->text_slots.data[i];
    VkrText3D *text = &slot->text;
    if (!slot->active)
      continue;
    if (!vkr_text_3d_prepare_geometry(text))
      return false_v;
    if (text->index_count == 0u)
      continue;
    VkrFont *font =
        vkr_font_system_get_by_handle(text->font_system, text->font);
    if (!font)
      font = vkr_font_system_get_default_mtsdf_font(text->font_system);
    if (!font || font->atlas.id == 0 ||
        font->atlas.generation == VKR_INVALID_ID)
      return false_v;
    Vec2 unit_range = {0};
    uint32_t font_mode = 0;
    if (font->type == VKR_FONT_TYPE_MTSDF) {
      font_mode = 1;
      unit_range = font->mtsdf_unit_range;
    }
    Mat4 model = vkr_transform_get_world(&text->transform);
    if (text->texture_width > 0 && text->texture_height > 0) {
      model = mat4_mul(
          model, mat4_scale(vec3_new(text->world_width / text->texture_width,
                                     text->world_height / text->texture_height,
                                     1.0f)));
    }
    draws[count] = (VkrPreparedTextDraw){
        .vertices = text->vertices,
        .vertex_count = text->vertex_count,
        .indices = text->indices,
        .index_count = text->index_count,
        .max_index = text->vertex_count - 1u,
        .atlas = font->atlas,
        .model = model,
        .unit_range = unit_range,
        .font_mode = font_mode,
        .object_id =
            vkr_picking_encode_id(VKR_PICKING_ID_KIND_WORLD_TEXT, (uint32_t)i),
        .revision = text->geometry_revision,
    };
    count++;
  }
  *out_draws = count > 0u ? draws : NULL;
  *out_count = count;
  return true_v;
}
