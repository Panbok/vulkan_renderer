/**
 * @file vkr_world_resources.c
 * @brief Fallback IBL resources, scene bake preparation, and 3D text slots.
 */

#include "renderer/systems/vkr_world_resources.h"

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
#include "renderer/vkr_frame_input.h"
#include "renderer/vkr_ibl_math.h"

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
         assets->asset_publisher->bake_hdr_environment &&
         assets->asset_publisher->ibl_sh_slot;
}

vkr_internal uint32_t
vkr_world_resources_ibl_mip_limit(const VkrWorldResources *resources) {
  return resources ? Min(resources->hdr_ibl_max_mip_levels,
                         (uint32_t)VKR_IBL_PREFILTER_MIP_COUNT)
                   : 0u;
}

vkr_internal bool8_t vkr_world_resources_create_writable_cube_texture(
    VkrRenderAssets *assets, String8 name, uint32_t size, bool8_t with_mips,
    VkrTextureFormat format, VkrTextureHandle *out_handle) {
  if (!assets || !name.str || !out_handle || size == 0) {
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
  if (!vkr_texture_system_create_writable(&assets->texture_system, name, &desc,
                                          out_handle, &texture_err)) {
    String8 err = vkr_renderer_get_error_string(texture_err);
    log_warn("World resources: failed to create writable cubemap '%.*s': %s",
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
  resources->ibl_fallback_source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->hdr_ibl_max_cube_extent = VKR_IBL_PREFILTER_SIZE;
  resources->hdr_ibl_max_mip_levels = VKR_IBL_PREFILTER_MIP_COUNT;
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
  vkr_world_resources_release_texture(&assets->texture_system,
                                      &environment->delivery_equirect);
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
}

vkr_internal void vkr_world_resources_retain_first_scene_environment_as_default(
    VkrRenderAssets *assets, VkrWorldResources *resources,
    const VkrSceneEnvironment *environment) {
  if (resources->ibl_default_ready) {
    return;
  }

  vkr_texture_system_add_ref_by_handle(&assets->texture_system,
                                       environment->source_cubemap);
  vkr_texture_system_add_ref_by_handle(&assets->texture_system,
                                       environment->prefilter_cubemap);
  resources->ibl_fallback_source_cubemap = environment->source_cubemap;
  resources->ibl_fallback_prefilter_cubemap = environment->prefilter_cubemap;
  resources->ibl_default_ready = true_v;
}

vkr_internal bool8_t vkr_world_resources_prepare_published_environment(
    VkrRenderAssets *assets, VkrWorldResources *resources, VkrScene *scene) {
  VkrSceneEnvironment *environment = &scene->environment;
  if (environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT) {
    VkrTexture *delivery = vkr_texture_system_get_by_handle(
        &assets->texture_system, environment->delivery_equirect);
    if (!delivery || !delivery->handle ||
        delivery->description.type != VKR_TEXTURE_TYPE_2D ||
        delivery->description.format !=
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT ||
        !assets->asset_publisher->bake_hdr_environment ||
        !vkr_ibl_derive_cubemap_size(
            delivery->description.width, delivery->description.height,
            resources->hdr_ibl_max_cube_extent,
            vkr_world_resources_ibl_mip_limit(resources),
            &environment->source_face_size, &environment->source_mip_count)) {
      goto failed;
    }
  } else if (environment->source_kind == VKR_SCENE_ENV_SOURCE_CUBEMAP) {
    VkrTexture *source = vkr_texture_system_get_by_handle(
        &assets->texture_system, environment->source_cubemap);
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
  char prefilter_name_storage[128];
  snprintf(source_name_storage, sizeof(source_name_storage),
           "__ibl.scene.%p.source", (void *)scene);
  snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
           "__ibl.scene.%p.prefilter", (void *)scene);
  const String8 source_name = string8_create_from_cstr(
      (const uint8_t *)source_name_storage, string_length(source_name_storage));
  const String8 prefilter_name =
      string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                               string_length(prefilter_name_storage));

  if ((environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT &&
       !vkr_world_resources_create_writable_cube_texture(
           assets, source_name, environment->source_face_size, true_v,
           VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
           &environment->source_cubemap)) ||
      !vkr_world_resources_create_writable_cube_texture(
          assets, prefilter_name, VKR_IBL_PREFILTER_SIZE, true_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          &environment->prefilter_cubemap)) {
    goto failed;
  }

  const bool8_t baked =
      environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT
          ? assets->asset_publisher->bake_hdr_environment(
                assets->asset_publisher->state, environment->delivery_equirect,
                environment->source_cubemap, environment->prefilter_cubemap,
                environment->sh_deringing)
          : assets->asset_publisher->bake_ibl_cubemap(
                assets->asset_publisher->state, environment->source_cubemap,
                environment->prefilter_cubemap, environment->sh_deringing);
  if (!baked) {
    goto failed;
  }
  (void)vkr_world_resources_release_texture(&assets->texture_system,
                                            &environment->delivery_equirect);
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_READY;
  vkr_world_resources_retain_first_scene_environment_as_default(
      assets, resources, environment);
  return true_v;

failed:
  vkr_world_resources_fail_scene_environment(assets, scene);
  return false_v;
}

bool8_t vkr_world_resources_prepare_scene_environment(
    VkrRenderAssets *assets, VkrWorldResources *resources, VkrScene *scene) {
  if (!assets || !resources || !scene || !scene->environment.enabled ||
      !vkr_world_resources_has_retained_ibl_publisher(assets)) {
    if (assets && scene) {
      vkr_world_resources_fail_scene_environment(assets, scene);
    }
    return false_v;
  }
  return vkr_world_resources_prepare_published_environment(assets, resources,
                                                           scene);
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
    if (!vkr_world_resources_create_writable_cube_texture(
            assets, prefilter_name, VKR_IBL_PREFILTER_SIZE, true_v,
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

  vkr_world_resources_release_texture(
      &assets->texture_system, &resources->ibl_fallback_prefilter_cubemap);
  vkr_world_resources_release_texture(&assets->texture_system,
                                      &resources->ibl_fallback_source_cubemap);
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
