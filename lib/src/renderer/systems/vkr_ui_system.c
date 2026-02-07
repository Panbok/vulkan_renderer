/**
 * @file vkr_ui_system.c
 * @brief Stateless UI text and pipeline resources.
 */

#include "renderer/systems/vkr_ui_system.h"

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_window.h"
#include "math/mat.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"

#define VKR_UI_SYSTEM_MAX_TEXTS 16

vkr_internal bool8_t vkr_ui_system_get_layout_size(RendererFrontend *rf,
                                                   VkrUiSystem *system,
                                                   uint32_t *out_width,
                                                   uint32_t *out_height) {
  if (!rf || !system || !out_width || !out_height) {
    return false_v;
  }

  uint32_t width = rf->last_window_width;
  uint32_t height = rf->last_window_height;
  if (width == 0 || height == 0) {
    VkrWindowPixelSize size = vkr_window_get_pixel_size(rf->window);
    width = size.width;
    height = size.height;
  }

  if (system->offscreen_enabled && system->offscreen_width > 0 &&
      system->offscreen_height > 0) {
    *out_width = system->offscreen_width;
    *out_height = system->offscreen_height;
    return true_v;
  }

  *out_width = width;
  *out_height = height;
  return true_v;
}

vkr_internal void vkr_ui_system_position_slot(VkrUiTextSlot *slot,
                                              uint32_t width, uint32_t height);

vkr_internal void vkr_ui_system_refresh_layout(RendererFrontend *rf,
                                               VkrUiSystem *system) {
  if (!rf || !system) {
    return;
  }

  uint32_t prev_width = system->screen_width;
  uint32_t prev_height = system->screen_height;
  uint32_t layout_width = 0;
  uint32_t layout_height = 0;
  if (!vkr_ui_system_get_layout_size(rf, system, &layout_width,
                                     &layout_height)) {
    return;
  }

  if (layout_width == prev_width && layout_height == prev_height) {
    return;
  }

  system->screen_width = layout_width;
  system->screen_height = layout_height;

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_ui_system_position_slot(slot, layout_width, layout_height);
  }
}

vkr_internal void vkr_ui_system_position_slot(VkrUiTextSlot *slot,
                                              uint32_t width, uint32_t height) {
  if (!slot || !slot->active || width == 0 || height == 0) {
    return;
  }

  VkrTextBounds bounds = vkr_ui_text_get_bounds(&slot->text);
  float32_t x = slot->padding.x;
  float32_t y = slot->padding.y;

  switch (slot->anchor) {
  case VKR_UI_TEXT_ANCHOR_TOP_RIGHT:
    x = (float32_t)width - bounds.size.x - slot->padding.x;
    y = (float32_t)height - bounds.size.y - slot->padding.y;
    break;
  case VKR_UI_TEXT_ANCHOR_BOTTOM_LEFT:
    y = slot->padding.y;
    break;
  case VKR_UI_TEXT_ANCHOR_BOTTOM_RIGHT:
    x = (float32_t)width - bounds.size.x - slot->padding.x;
    y = slot->padding.y;
    break;
  case VKR_UI_TEXT_ANCHOR_TOP_LEFT:
  default:
    x = slot->padding.x;
    y = (float32_t)height - bounds.size.y - slot->padding.y;
    break;
  }

  vkr_transform_set_position(&slot->text.transform, vec3_new(x, y, 0.0f));
}

vkr_internal bool8_t vkr_ui_system_ensure_slot(VkrUiSystem *system,
                                               uint32_t text_id,
                                               VkrUiTextSlot **out_slot) {
  if (!system || !out_slot || !system->text_slots.data) {
    return false_v;
  }
  if (text_id >= system->text_slots.length) {
    log_error("UI text id %u exceeds max (%llu)", text_id,
              (unsigned long long)system->text_slots.length);
    return false_v;
  }

  *out_slot = &system->text_slots.data[text_id];
  return true_v;
}

vkr_internal bool8_t vkr_ui_system_find_free_slot(VkrUiSystem *system,
                                                  uint32_t *out_text_id,
                                                  VkrUiTextSlot **out_slot) {
  if (!system || !out_text_id || !out_slot || !system->text_slots.data) {
    return false_v;
  }

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active) {
      *out_text_id = (uint32_t)i;
      *out_slot = slot;
      return true_v;
    }
  }

  log_error("UI text slots exhausted (max %llu)",
            (unsigned long long)system->text_slots.length);
  return false_v;
}

vkr_internal VkrUiTextSlot *vkr_ui_system_get_active_slot(VkrUiSystem *system,
                                                          uint32_t text_id) {
  if (!system || !system->text_slots.data ||
      text_id >= system->text_slots.length) {
    return NULL;
  }

  VkrUiTextSlot *slot = &system->text_slots.data[text_id];
  return slot->active ? slot : NULL;
}

bool8_t vkr_ui_system_init(RendererFrontend *rf, VkrUiSystem *system) {
  if (!rf || !system) {
    return false_v;
  }

  MemZero(system, sizeof(*system));
  system->instance_state.id = VKR_INVALID_ID;

  VkrResourceHandleInfo ui_cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.ui.shadercfg"),
          &rf->scratch_allocator, &ui_cfg_info, &shadercfg_err)) {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("UI shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }
  system->shader_config = *(VkrShaderConfig *)ui_cfg_info.as.custom;

  if (!vkr_shader_system_create(&rf->shader_system, &system->shader_config)) {
    log_error("Failed to create UI shader in shader system");
    return false_v;
  }

  VkrRendererError pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &system->shader_config,
          VKR_PIPELINE_DOMAIN_UI, string8_lit("ui"), &system->pipeline,
          &pipeline_err)) {
    String8 err_str = vkr_renderer_get_error_string(pipeline_err);
    log_error("UI pipeline creation failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  VkrResourceHandleInfo material_info = {0};
  VkrRendererError material_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL,
                               string8_lit("assets/materials/default.ui.mt"),
                               &rf->scratch_allocator, &material_info,
                               &material_err)) {
    system->material = material_info.as.material;
  } else {
    String8 err_str = vkr_renderer_get_error_string(material_err);
    log_warn("Default UI material load failed: %s", string8_cstr(&err_str));
  }

  VkrRendererError instance_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, system->pipeline, &system->instance_state,
          &instance_err)) {
    String8 err_str = vkr_renderer_get_error_string(instance_err);
    log_error("UI instance state acquire failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  VkrResourceHandleInfo text_cfg_info = {0};
  VkrRendererError text_shader_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.text.shadercfg"),
          &rf->scratch_allocator, &text_cfg_info, &text_shader_err)) {
    String8 err = vkr_renderer_get_error_string(text_shader_err);
    log_error("UI text shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  system->text_shader_config = *(VkrShaderConfig *)text_cfg_info.as.custom;
  if (!vkr_shader_system_create(&rf->shader_system,
                                &system->text_shader_config)) {
    log_error("Failed to create UI text shader in shader system");
    return false_v;
  }

  VkrRendererError text_pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &system->text_shader_config,
          VKR_PIPELINE_DOMAIN_UI, string8_lit("ui_text"),
          &system->text_pipeline, &text_pipeline_err)) {
    String8 err_str = vkr_renderer_get_error_string(text_pipeline_err);
    log_error("UI text pipeline creation failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  system->text_slots =
      array_create_VkrUiTextSlot(&rf->allocator, VKR_UI_SYSTEM_MAX_TEXTS);
  MemZero(system->text_slots.data,
          sizeof(VkrUiTextSlot) * (uint64_t)system->text_slots.length);

  system->screen_width = rf->last_window_width;
  system->screen_height = rf->last_window_height;
  system->initialized = true_v;
  return true_v;
}

void vkr_ui_system_shutdown(RendererFrontend *rf, VkrUiSystem *system) {
  if (!rf || !system) {
    return;
  }

  if (system->instance_state.id != VKR_INVALID_ID && system->pipeline.id != 0) {
    vkr_pipeline_registry_release_instance_state(
        &rf->pipeline_registry, system->pipeline, system->instance_state,
        &(VkrRendererError){0});
    system->instance_state.id = VKR_INVALID_ID;
  }

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (slot->active) {
      vkr_ui_text_destroy(&slot->text);
      slot->active = false_v;
    }
  }
  array_destroy_VkrUiTextSlot(&system->text_slots);

  if (system->text_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           system->text_pipeline);
    system->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (system->pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           system->pipeline);
    system->pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  system->initialized = false_v;
}

void vkr_ui_system_resize(RendererFrontend *rf, VkrUiSystem *system,
                          uint32_t width, uint32_t height) {
  if (!rf || !system) {
    return;
  }

  rf->globals.ui_view = mat4_identity();
  rf->globals.ui_projection =
      mat4_ortho(0.0f, (float32_t)width, (float32_t)height, 0.0f, -1.0f, 1.0f);
  vkr_pipeline_registry_mark_global_state_dirty(&rf->pipeline_registry);

  uint32_t layout_width = width;
  uint32_t layout_height = height;
  if (system->offscreen_enabled && system->offscreen_width > 0 &&
      system->offscreen_height > 0) {
    layout_width = system->offscreen_width;
    layout_height = system->offscreen_height;
  }

  system->screen_width = layout_width;
  system->screen_height = layout_height;

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_ui_system_position_slot(slot, layout_width, layout_height);
  }
}

void vkr_ui_system_set_offscreen_size(RendererFrontend *rf, VkrUiSystem *system,
                                      bool8_t enabled, uint32_t width,
                                      uint32_t height) {
  if (!rf || !system) {
    return;
  }

  system->offscreen_enabled = enabled;
  system->offscreen_width = width;
  system->offscreen_height = height;
}

bool8_t vkr_ui_system_text_create(RendererFrontend *rf, VkrUiSystem *system,
                                  const VkrUiTextCreateData *payload,
                                  uint32_t *out_text_id) {
  if (!rf || !system || !payload) {
    return false_v;
  }

  if (system->text_pipeline.id == 0) {
    log_error("UI text pipeline not initialized");
    return false_v;
  }

  uint32_t text_id = payload->text_id;
  VkrUiTextSlot *slot = NULL;
  if (text_id == VKR_INVALID_ID) {
    if (!vkr_ui_system_find_free_slot(system, &text_id, &slot)) {
      return false_v;
    }
  } else {
    if (!vkr_ui_system_ensure_slot(system, text_id, &slot)) {
      return false_v;
    }
  }

  if (slot->active) {
    vkr_ui_text_destroy(&slot->text);
    slot->active = false_v;
  }

  const VkrUiTextConfig *config = payload->config;
  VkrRendererError text_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_ui_text_create(rf, &rf->allocator, &rf->font_system,
                          system->text_pipeline, payload->content, config,
                          &slot->text, &text_err)) {
    String8 err = vkr_renderer_get_error_string(text_err);
    log_error("Failed to create UI text: %s", string8_cstr(&err));
    return false_v;
  }

  slot->active = true_v;
  slot->anchor = payload->anchor;
  slot->padding = payload->padding;

  uint32_t layout_width = 0;
  uint32_t layout_height = 0;
  vkr_ui_system_get_layout_size(rf, system, &layout_width, &layout_height);
  vkr_ui_system_position_slot(slot, layout_width, layout_height);

  if (out_text_id) {
    *out_text_id = text_id;
  }
  return true_v;
}

bool8_t vkr_ui_system_text_update(RendererFrontend *rf, VkrUiSystem *system,
                                  uint32_t text_id, String8 content) {
  if (!system) {
    return false_v;
  }

  VkrUiTextSlot *slot = vkr_ui_system_get_active_slot(system, text_id);
  if (!slot) {
    log_warn("UI text id %u not found for update", text_id);
    return false_v;
  }

  if (!vkr_ui_text_set_content(&slot->text, content)) {
    log_error("Failed to update UI text content");
    return false_v;
  }

  uint32_t layout_width = 0;
  uint32_t layout_height = 0;
  vkr_ui_system_get_layout_size(rf, system, &layout_width, &layout_height);
  vkr_ui_system_position_slot(slot, layout_width, layout_height);
  return true_v;
}

bool8_t vkr_ui_system_text_destroy(RendererFrontend *rf, VkrUiSystem *system,
                                   uint32_t text_id) {
  (void)rf;

  if (!system) {
    return false_v;
  }

  VkrUiTextSlot *slot = vkr_ui_system_get_active_slot(system, text_id);
  if (!slot) {
    log_warn("UI text id %u not found for destroy", text_id);
    return false_v;
  }

  vkr_ui_text_destroy(&slot->text);
  slot->active = false_v;
  return true_v;
}

void vkr_ui_system_render_text(RendererFrontend *rf, VkrUiSystem *system) {
  if (!rf || !system) {
    return;
  }

  vkr_ui_system_refresh_layout(rf, system);

  bool8_t override_projection = system->offscreen_enabled &&
                                system->offscreen_width > 0 &&
                                system->offscreen_height > 0;
  Mat4 prev_view = rf->globals.ui_view;
  Mat4 prev_proj = rf->globals.ui_projection;
  if (override_projection) {
    rf->globals.ui_view = mat4_identity();
    rf->globals.ui_projection =
        mat4_ortho(0.0f, (float32_t)system->offscreen_width,
                   (float32_t)system->offscreen_height, 0.0f, -1.0f, 1.0f);
  }

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_ui_text_draw(&slot->text);
  }

  if (override_projection) {
    rf->globals.ui_view = prev_view;
    rf->globals.ui_projection = prev_proj;
  }
}

void vkr_ui_system_render_picking_text(RendererFrontend *rf,
                                       VkrUiSystem *system,
                                       VkrPipelineHandle pipeline) {
  if (!rf || !system || pipeline.id == 0) {
    return;
  }

  vkr_ui_system_refresh_layout(rf, system);

  if (!vkr_shader_system_use(&rf->shader_system, "shader.picking_text")) {
    log_warn("Failed to use picking text shader for UI");
    return;
  }

  VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, pipeline,
                                           &bind_err)) {
    String8 err_str = vkr_renderer_get_error_string(bind_err);
    log_warn("Failed to bind picking text pipeline for UI: %s",
             string8_cstr(&err_str));
    return;
  }

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_UI);

  for (uint64_t i = 0; i < system->text_slots.length; ++i) {
    VkrUiTextSlot *slot = &system->text_slots.data[i];
    if (!slot->active) {
      continue;
    }

    if (!vkr_ui_text_prepare(&slot->text)) {
      continue;
    }

    if (slot->text.render.quad_count == 0) {
      continue;
    }

    uint32_t object_id =
        vkr_picking_encode_id(VKR_PICKING_ID_KIND_UI_TEXT, (uint32_t)i);
    if (object_id == 0) {
      continue;
    }

    Mat4 model = vkr_transform_get_world(&slot->text.transform);
    vkr_material_system_apply_local(
        &rf->material_system,
        &(VkrLocalMaterialState){.model = model, .object_id = object_id});

    if (!vkr_shader_system_apply_instance(&rf->shader_system)) {
      continue;
    }

    VkrVertexBufferBinding vbb = {
        .buffer = slot->text.render.vertex_buffer.handle,
        .binding = 0,
        .offset = 0,
    };
    vkr_renderer_bind_vertex_buffer(rf, &vbb);

    VkrIndexBufferBinding ibb = {
        .buffer = slot->text.render.index_buffer.handle,
        .type = VKR_INDEX_TYPE_UINT32,
        .offset = 0,
    };
    vkr_renderer_bind_index_buffer(rf, &ibb);

    uint32_t index_count = slot->text.render.quad_count * 6;
    vkr_renderer_draw_indexed(rf, index_count, 1, 0, 0, 0);
  }
}
