/**
 * @file vkr_world_resources.c
 * @brief Stateless world pipelines and 3D text resources.
 */

#include "renderer/systems/vkr_world_resources.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"

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

bool8_t vkr_world_resources_init(RendererFrontend *rf,
                                 VkrWorldResources *resources) {
  if (!rf || !resources) {
    return false_v;
  }

  MemZero(resources, sizeof(*resources));

  VkrResourceHandleInfo world_cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.world.shadercfg"),
          &rf->scratch_allocator, &world_cfg_info, &shadercfg_err)) {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("World shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  resources->shader_config = *(VkrShaderConfig *)world_cfg_info.as.custom;
  if (!vkr_shader_system_create(&rf->shader_system,
                                &resources->shader_config)) {
    log_error("Failed to create world shader in shader system");
    return false_v;
  }

  VkrRendererError pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &resources->shader_config,
          VKR_PIPELINE_DOMAIN_WORLD, string8_lit("world"),
          &resources->pipeline, &pipeline_err)) {
    String8 err_str = vkr_renderer_get_error_string(pipeline_err);
    log_error("World pipeline creation failed: %s", string8_cstr(&err_str));
    goto cleanup;
  }

  VkrRendererError transparent_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &resources->shader_config,
          VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT,
          string8_lit("world_transparent"), &resources->transparent_pipeline,
          &transparent_err)) {
    String8 err_str = vkr_renderer_get_error_string(transparent_err);
    log_error("World transparent pipeline creation failed: %s",
              string8_cstr(&err_str));
    goto cleanup;
  }

  VkrRendererError overlay_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &resources->shader_config,
          VKR_PIPELINE_DOMAIN_WORLD_OVERLAY, string8_lit("world_overlay"),
          &resources->overlay_pipeline, &overlay_err)) {
    String8 err_str = vkr_renderer_get_error_string(overlay_err);
    log_warn("World overlay pipeline creation failed: %s",
             string8_cstr(&err_str));
    resources->overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  VkrResourceHandleInfo text_cfg_info = {0};
  VkrRendererError text_cfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.world_text.shadercfg"),
          &rf->scratch_allocator, &text_cfg_info, &text_cfg_err)) {
    String8 err = vkr_renderer_get_error_string(text_cfg_err);
    log_error("World text shadercfg load failed: %s", string8_cstr(&err));
    goto cleanup;
  }

  resources->text_shader_config = *(VkrShaderConfig *)text_cfg_info.as.custom;
  if (!vkr_shader_system_create(&rf->shader_system,
                                &resources->text_shader_config)) {
    log_error("Failed to create world text shader in shader system");
    goto cleanup;
  }

  VkrShaderConfig text_cfg = resources->text_shader_config;
  text_cfg.cull_mode = VKR_CULL_MODE_NONE;

  VkrRendererError text_pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &text_cfg,
          VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT, string8_lit("world_text_3d"),
          &resources->text_pipeline, &text_pipeline_err)) {
    String8 err_str = vkr_renderer_get_error_string(text_pipeline_err);
    log_warn("World text pipeline creation failed: %s",
             string8_cstr(&err_str));
    resources->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  resources->text_slots = array_create_VkrWorldTextSlot(
      &rf->allocator, VKR_WORLD_RESOURCES_MAX_TEXTS);
  if (!resources->text_slots.data) {
    log_error("World text slots array create failed");
    goto cleanup;
  }
  MemZero(resources->text_slots.data,
          sizeof(VkrWorldTextSlot) * (uint64_t)resources->text_slots.length);

  resources->initialized = true_v;
  return true_v;

cleanup:
  if (resources->text_slots.data) {
    array_destroy_VkrWorldTextSlot(&resources->text_slots);
    resources->text_slots = (Array_VkrWorldTextSlot){0};
  }
  if (resources->text_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->text_pipeline);
    resources->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->overlay_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->overlay_pipeline);
    resources->overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->transparent_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->transparent_pipeline);
    resources->transparent_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->pipeline);
    resources->pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  MemZero(&resources->shader_config, sizeof(resources->shader_config));
  MemZero(&resources->text_shader_config, sizeof(resources->text_shader_config));
  return false_v;
}

void vkr_world_resources_shutdown(RendererFrontend *rf,
                                  VkrWorldResources *resources) {
  if (!rf || !resources) {
    return;
  }

  for (uint64_t i = 0; i < resources->text_slots.length; ++i) {
    VkrWorldTextSlot *slot = &resources->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_text_3d_destroy(&slot->text);
    slot->active = false_v;
  }
  array_destroy_VkrWorldTextSlot(&resources->text_slots);

  if (resources->text_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->text_pipeline);
    resources->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (resources->overlay_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->overlay_pipeline);
    resources->overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (resources->transparent_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->transparent_pipeline);
    resources->transparent_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (resources->pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->pipeline);
    resources->pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  resources->initialized = false_v;
}

bool8_t vkr_world_resources_text_create(RendererFrontend *rf,
                                        VkrWorldResources *resources,
                                        const VkrWorldTextCreateData *payload) {
  if (!rf || !resources || !payload) {
    return false_v;
  }

  if (resources->text_pipeline.id == 0) {
    log_error("World text pipeline not ready");
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
  config.pipeline = resources->text_pipeline;

  VkrRendererError text_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_text_3d_create(&slot->text, rf, &rf->font_system, &rf->allocator,
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

  VkrWorldTextSlot *slot = vkr_world_resources_get_text_slot(resources, text_id);
  if (!slot) {
    log_warn("World text id %u not found for update", text_id);
    return false_v;
  }

  vkr_text_3d_set_text(&slot->text, content);
  return true_v;
}

bool8_t vkr_world_resources_text_set_transform(
    RendererFrontend *rf, VkrWorldResources *resources, uint32_t text_id,
    const VkrTransform *transform) {
  (void)rf;
  if (!resources || !transform) {
    return false_v;
  }

  VkrWorldTextSlot *slot = vkr_world_resources_get_text_slot(resources, text_id);
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

  VkrWorldTextSlot *slot = vkr_world_resources_get_text_slot(resources, text_id);
  if (!slot) {
    log_warn("World text id %u not found for destroy", text_id);
    return false_v;
  }

  vkr_text_3d_destroy(&slot->text);
  slot->active = false_v;
  return true_v;
}

void vkr_world_resources_render_text(RendererFrontend *rf,
                                     VkrWorldResources *resources) {
  if (!rf || !resources || !resources->text_slots.data) {
    return;
  }

  for (uint64_t i = 0; i < resources->text_slots.length; ++i) {
    VkrWorldTextSlot *slot = &resources->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_text_3d_draw(&slot->text);
  }
}

void vkr_world_resources_render_picking_text(RendererFrontend *rf,
                                             VkrWorldResources *resources,
                                             VkrPipelineHandle pipeline) {
  if (!rf || !resources || pipeline.id == 0) {
    return;
  }
  if (!resources->text_slots.data) {
    return;
  }

  if (!vkr_shader_system_use(&rf->shader_system, "shader.picking_text")) {
    log_warn("Failed to use picking text shader for world");
    return;
  }

  VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, pipeline,
                                           &bind_err)) {
    String8 err_str = vkr_renderer_get_error_string(bind_err);
    log_warn("Failed to bind picking text pipeline for world: %s",
             string8_cstr(&err_str));
    return;
  }

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_WORLD);

  for (uint64_t i = 0; i < resources->text_slots.length; ++i) {
    VkrWorldTextSlot *slot = &resources->text_slots.data[i];
    if (!slot->active) {
      continue;
    }

    vkr_text_3d_update(&slot->text);
    if (slot->text.quad_count == 0) {
      continue;
    }

    uint32_t object_id =
        vkr_picking_encode_id(VKR_PICKING_ID_KIND_WORLD_TEXT, (uint32_t)i);
    if (object_id == 0) {
      continue;
    }

    Mat4 model = vkr_transform_get_world(&slot->text.transform);
    if (slot->text.texture_width > 0 && slot->text.texture_height > 0) {
      Vec3 scale = vec3_new(
          slot->text.world_width / (float32_t)slot->text.texture_width,
          slot->text.world_height / (float32_t)slot->text.texture_height, 1.0f);
      model = mat4_mul(model, mat4_scale(scale));
    }

    vkr_material_system_apply_local(
        &rf->material_system,
        &(VkrLocalMaterialState){.model = model, .object_id = object_id});

    if (!vkr_shader_system_apply_instance(&rf->shader_system)) {
      continue;
    }

    uint64_t idx64 = (uint64_t)slot->text.quad_count * 6u;
    if (idx64 > (uint64_t)UINT32_MAX) {
      log_error("World text index count overflow (quad_count=%u)",
                slot->text.quad_count);
      continue;
    }
    uint32_t index_count = (uint32_t)idx64;

    VkrVertexBufferBinding vbb = {
        .buffer = slot->text.vertex_buffer.handle,
        .binding = 0,
        .offset = 0,
    };
    vkr_renderer_bind_vertex_buffer(rf, &vbb);

    VkrIndexBufferBinding ibb = {
        .buffer = slot->text.index_buffer.handle,
        .type = VKR_INDEX_TYPE_UINT32,
        .offset = 0,
    };
    vkr_renderer_bind_index_buffer(rf, &ibb);

    vkr_renderer_draw_indexed(rf, index_count, 1, 0, 0, 0);
  }
}
