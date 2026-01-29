/**
 * @file vkr_view_ui.c
 * @brief UI view layer implementation.
 *
 * The UI layer handles 2D user interface rendering including:
 * - Screen-space text with anchor-based positioning
 * - Support for offscreen rendering (editor mode)
 * - Orthographic projection for pixel-perfect rendering
 *
 * In editor mode, the UI layer renders to offscreen targets shared with
 * the World and Skybox layers, allowing the composite scene to be displayed
 * in the editor viewport.
 */

#include "renderer/systems/views/vkr_view_ui.h"

#include "containers/array.h"
#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vkr_transform.h"
#include "memory/vkr_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/resources/ui/vkr_ui_text.h"
#include "renderer/systems/vkr_layer_messages.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_view_system.h"
#include "renderer/vulkan/vulkan_types.h"

/** Maximum number of UI text objects per layer. */
#define VKR_VIEW_UI_MAX_TEXTS 16

/** Offscreen renderpass name for UI compositing in editor mode. */
#define VKR_VIEW_OFFSCREEN_UI_PASS_NAME "Renderpass.Offscreen.UI"

vkr_internal VkrTextureFormat vkr_view_ui_get_swapchain_format(
    RendererFrontend *rf) {
  if (!rf) {
    return VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM;
  }

  VkrTextureOpaqueHandle swapchain_tex =
      vkr_renderer_window_attachment_get(rf, 0);
  if (!swapchain_tex) {
    return VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM;
  }

  struct s_TextureHandle *handle = (struct s_TextureHandle *)swapchain_tex;
  return handle->description.format;
}

/**
 * @brief Slot for a UI text object with anchor-based positioning.
 */
typedef struct VkrViewUiTextSlot {
  VkrUiText text;             /**< The UI text instance. */
  bool8_t active;             /**< Whether this slot is in use. */
  VkrViewUiTextAnchor anchor; /**< Screen corner anchor point. */
  Vec2 padding;               /**< Offset from anchor in pixels. */
} VkrViewUiTextSlot;
Array(VkrViewUiTextSlot);

/**
 * @brief Internal state for the UI view layer.
 *
 * Contains resources for UI rendering including pipelines for both
 * swapchain and offscreen modes, and a pool of text slots.
 */
typedef struct VkrViewUIState {
  /* UI rendering (general) */
  VkrShaderConfig shader_config;                 /**< UI shader config. */
  VkrPipelineHandle pipeline;                    /**< UI rendering pipeline. */
  VkrMaterialHandle material;                    /**< Default UI material. */
  VkrRendererInstanceStateHandle instance_state; /**< Pipeline instance. */

  /* Offscreen rendering */
  VkrRenderPassHandle offscreen_renderpass;  /**< Offscreen UI pass. */
  VkrRenderTargetHandle *offscreen_targets;  /**< Per-swapchain targets. */
  VkrTextureOpaqueHandle *offscreen_colors;  /**< Color attachments. */
  VkrTextureLayout *offscreen_color_layouts; /**< Layout tracking. */
  uint32_t offscreen_count;  /**< Number of targets (swapchain count). */
  uint32_t offscreen_width;  /**< Offscreen target width. */
  uint32_t offscreen_height; /**< Offscreen target height. */
  bool8_t offscreen_enabled; /**< Whether offscreen mode is active. */

  /* Text rendering */
  VkrShaderConfig text_shader_config;        /**< Text shader config. */
  VkrPipelineHandle text_pipeline;           /**< Swapchain text pipeline. */
  VkrPipelineHandle text_pipeline_offscreen; /**< Offscreen text pipeline. */
  Array_VkrViewUiTextSlot text_slots;        /**< Pool of text slots. */
  uint32_t screen_width;  /**< Current screen/viewport width. */
  uint32_t screen_height; /**< Current screen/viewport height. */
} VkrViewUIState;

/* ============================================================================
 * Layer Lifecycle Callbacks
 * ============================================================================
 */
vkr_internal bool32_t vkr_view_ui_on_create(VkrLayerContext *ctx);
vkr_internal void vkr_view_ui_on_attach(VkrLayerContext *ctx);
vkr_internal void vkr_view_ui_on_resize(VkrLayerContext *ctx, uint32_t width,
                                        uint32_t height);
vkr_internal void vkr_view_ui_on_render(VkrLayerContext *ctx,
                                        const VkrLayerRenderInfo *info);
vkr_internal void vkr_view_ui_on_detach(VkrLayerContext *ctx);
vkr_internal void vkr_view_ui_on_destroy(VkrLayerContext *ctx);
vkr_internal void vkr_view_ui_on_data_received(VkrLayerContext *ctx,
                                               const VkrLayerMsgHeader *msg,
                                               void *out_rsp,
                                               uint64_t out_rsp_capacity,
                                               uint64_t *out_rsp_size);

/* ============================================================================
 * Layer Lookup
 * ============================================================================
 */

/** Finds the UI layer by handle in the view system. */
vkr_internal VkrLayer *vkr_view_ui_find_layer(VkrViewSystem *vs,
                                              VkrLayerHandle handle);

/* ============================================================================
 * Text Slot Management
 * ============================================================================
 */

/** Ensures a text slot exists at the given index. */
vkr_internal bool8_t vkr_view_ui_ensure_slot(VkrViewUIState *state,
                                             uint32_t text_id,
                                             VkrViewUiTextSlot **out_slot);

/** Finds an unused text slot and returns its ID. */
vkr_internal bool8_t vkr_view_ui_find_free_slot(VkrViewUIState *state,
                                                uint32_t *out_text_id,
                                                VkrViewUiTextSlot **out_slot);

/** Gets an active text slot by ID, or NULL if not found/inactive. */
vkr_internal VkrViewUiTextSlot *
vkr_view_ui_get_active_slot(VkrViewUIState *state, uint32_t text_id);

/** Updates a text slot's position based on its anchor and screen size. */
vkr_internal void vkr_view_ui_position_slot(VkrViewUiTextSlot *slot,
                                            uint32_t width, uint32_t height);

/** Gets the current screen/viewport size for text positioning. */
vkr_internal bool8_t vkr_view_ui_get_screen_size(VkrLayerContext *ctx,
                                                 VkrViewUIState *state,
                                                 uint32_t *out_width,
                                                 uint32_t *out_height);

/** Rebuilds all active text objects with a new pipeline. */
vkr_internal void vkr_view_ui_rebuild_texts(RendererFrontend *rf,
                                            VkrViewUIState *state,
                                            VkrPipelineHandle pipeline);

/* ============================================================================
 * Offscreen Target Management
 * ============================================================================
 */

/** Destroys offscreen render targets (framebuffers only, not textures). */
vkr_internal void vkr_view_ui_destroy_offscreen_targets(RendererFrontend *rf,
                                                        VkrViewUIState *state);

/** Creates offscreen render targets using provided color attachments. */
vkr_internal bool8_t vkr_view_ui_create_offscreen_targets(
    RendererFrontend *rf, VkrViewUIState *state, VkrTextureOpaqueHandle *colors,
    uint32_t count);

bool32_t vkr_view_ui_register(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (!rf->view_system.initialized) {
    log_error("View system not initialized; cannot register UI view");
    return false_v;
  }

  if (rf->ui_layer.id != 0) {
    return true_v;
  }

  VkrLayerPassConfig ui_passes[1] = {{
      .renderpass_name = string8_lit("Renderpass.Builtin.UI"),
      .use_swapchain_color = true_v,
      .use_depth = false_v,
  }};

  VkrViewUIState *state = vkr_allocator_alloc(
      &rf->allocator, sizeof(VkrViewUIState), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!state) {
    log_error("Failed to allocate UI view state");
    return false_v;
  }
  MemZero(state, sizeof(*state));
  state->pipeline = VKR_PIPELINE_HANDLE_INVALID;
  state->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  state->text_pipeline_offscreen = VKR_PIPELINE_HANDLE_INVALID;

  VkrLayerConfig ui_cfg = {
      .name = string8_lit("Layer.UI"),
      .order = 1,
      .width = 0,
      .height = 0,
      .view = rf->globals.ui_view,
      .projection = rf->globals.ui_projection,
      .pass_count = ArrayCount(ui_passes),
      .passes = ui_passes,
      .callbacks = {.on_create = vkr_view_ui_on_create,
                    .on_attach = vkr_view_ui_on_attach,
                    .on_resize = vkr_view_ui_on_resize,
                    .on_render = vkr_view_ui_on_render,
                    .on_detach = vkr_view_ui_on_detach,
                    .on_destroy = vkr_view_ui_on_destroy,
                    .on_data_received = vkr_view_ui_on_data_received},
      .user_data = state,
      .enabled = true_v,
  };

  VkrRendererError layer_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_view_system_register_layer(rf, &ui_cfg, &rf->ui_layer, &layer_err)) {
    String8 err = vkr_renderer_get_error_string(layer_err);
    log_error("Failed to register UI view: %s", string8_cstr(&err));
    return false_v;
  }

  return true_v;
}

bool32_t vkr_view_ui_set_offscreen_enabled(
    RendererFrontend *rf, bool8_t enabled,
    VkrTextureOpaqueHandle *color_attachments, VkrTextureLayout *color_layouts,
    uint32_t attachment_count, uint32_t width, uint32_t height) {
  if (!rf) {
    return false_v;
  }

  VkrLayer *ui_layer = vkr_view_ui_find_layer(&rf->view_system, rf->ui_layer);
  if (!ui_layer || ui_layer->pass_count == 0) {
    return false_v;
  }

  VkrViewUIState *state = (VkrViewUIState *)ui_layer->user_data;
  if (!state) {
    return false_v;
  }

  VkrLayerPass *pass = array_get_VkrLayerPass(&ui_layer->passes, 0);
  if (enabled) {
    if (!color_attachments || attachment_count == 0) {
      log_error("Offscreen UI enabled without attachments");
      return false_v;
    }
    if (!state->offscreen_renderpass) {
      log_error("Offscreen UI renderpass not available");
      return false_v;
    }

    // Phase 4 hardening: avoid redundant rebuilds (and wait-idle stalls) when
    // caller re-applies the same offscreen configuration.
    if (state->offscreen_enabled && pass->use_custom_render_targets &&
        state->offscreen_width == width && state->offscreen_height == height &&
        state->offscreen_colors == color_attachments &&
        state->offscreen_color_layouts == color_layouts &&
        state->offscreen_count == attachment_count &&
        pass->render_targets == state->offscreen_targets &&
        pass->render_target_count == state->offscreen_count &&
        pass->custom_color_attachments == state->offscreen_colors &&
        pass->custom_color_layouts == state->offscreen_color_layouts &&
        pass->renderpass == state->offscreen_renderpass) {
      return true_v;
    }

    // Destroy old swapchain-backed framebuffers before switching to offscreen
    if (!pass->use_custom_render_targets && pass->render_targets &&
        pass->render_target_count > 0) {
      for (uint32_t i = 0; i < pass->render_target_count; ++i) {
        if (pass->render_targets[i]) {
          vkr_renderer_render_target_destroy(rf, pass->render_targets[i]);
        }
      }
      vkr_allocator_free(&rf->view_system.allocator, pass->render_targets,
                         sizeof(VkrRenderTargetHandle) *
                             (uint64_t)pass->render_target_count,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      pass->render_targets = NULL;
      pass->render_target_count = 0;
    }

    state->offscreen_width = width;
    state->offscreen_height = height;
    if (width > 0 && height > 0) {
      state->screen_width = width;
      state->screen_height = height;
    }
    if (!vkr_view_ui_create_offscreen_targets(rf, state, color_attachments,
                                              attachment_count)) {
      return false_v;
    }

    state->offscreen_color_layouts = color_layouts;
    state->offscreen_enabled = true_v;

    pass->use_custom_render_targets = true_v;
    pass->use_swapchain_color = false_v;
    pass->use_depth = false_v;
    pass->renderpass_name = string8_lit(VKR_VIEW_OFFSCREEN_UI_PASS_NAME);
    pass->renderpass = state->offscreen_renderpass;
    pass->render_targets = state->offscreen_targets;
    pass->render_target_count = state->offscreen_count;
    pass->custom_color_attachments = state->offscreen_colors;
    pass->custom_color_attachment_count = state->offscreen_count;
    pass->custom_color_layouts = state->offscreen_color_layouts;
    if (state->text_pipeline_offscreen.id != 0) {
      vkr_view_ui_rebuild_texts(rf, state, state->text_pipeline_offscreen);
    }
    return true_v;
  }

  state->offscreen_enabled = false_v;
  state->offscreen_width = 0;
  state->offscreen_height = 0;
  state->screen_width = rf->last_window_width;
  state->screen_height = rf->last_window_height;
  vkr_view_ui_destroy_offscreen_targets(rf, state);

  // Destroy old swapchain-backed framebuffers before switching
  if (!pass->use_custom_render_targets && pass->render_targets &&
      pass->render_target_count > 0) {
    for (uint32_t i = 0; i < pass->render_target_count; ++i) {
      if (pass->render_targets[i]) {
        vkr_renderer_render_target_destroy(rf, pass->render_targets[i]);
      }
    }
    vkr_allocator_free(&rf->view_system.allocator, pass->render_targets,
                       sizeof(VkrRenderTargetHandle) *
                           (uint64_t)pass->render_target_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  pass->use_custom_render_targets = false_v;
  pass->use_swapchain_color = true_v;
  pass->use_depth = false_v;
  pass->renderpass_name = string8_lit("Renderpass.Builtin.UI");
  pass->renderpass = NULL;
  pass->render_targets = NULL;
  pass->render_target_count = 0;
  pass->custom_color_attachments = NULL;
  pass->custom_color_layouts = NULL;
  if (state->text_pipeline.id != 0) {
    vkr_view_ui_rebuild_texts(rf, state, state->text_pipeline);
  }

  vkr_view_system_rebuild_targets(rf);
  return true_v;
}

void vkr_view_ui_render_picking_text(RendererFrontend *rf,
                                     VkrPipelineHandle pipeline) {
  if (!rf || !rf->view_system.initialized || pipeline.id == 0) {
    return;
  }

  VkrLayer *ui_layer = vkr_view_ui_find_layer(&rf->view_system, rf->ui_layer);
  if (!ui_layer || !ui_layer->enabled || !ui_layer->user_data) {
    return;
  }

  VkrViewUIState *state = (VkrViewUIState *)ui_layer->user_data;
  if (!state->text_slots.data) {
    return;
  }

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

  for (uint64_t i = 0; i < state->text_slots.length; ++i) {
    VkrViewUiTextSlot *slot = &state->text_slots.data[i];
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

vkr_internal bool8_t vkr_view_ui_ensure_slot(VkrViewUIState *state,
                                             uint32_t text_id,
                                             VkrViewUiTextSlot **out_slot) {
  if (!state || !out_slot) {
    return false_v;
  }

  if (!state->text_slots.data) {
    log_error("UI text slots not initialized");
    return false_v;
  }

  if (text_id >= state->text_slots.length) {
    log_error("UI text id %u exceeds max (%llu)", text_id,
              (unsigned long long)state->text_slots.length);
    return false_v;
  }

  *out_slot = &state->text_slots.data[text_id];
  return true_v;
}

vkr_internal bool8_t vkr_view_ui_find_free_slot(VkrViewUIState *state,
                                                uint32_t *out_text_id,
                                                VkrViewUiTextSlot **out_slot) {
  if (!state || !out_text_id || !out_slot) {
    return false_v;
  }

  if (!state->text_slots.data) {
    log_error("UI text slots not initialized");
    return false_v;
  }

  for (uint64_t i = 0; i < state->text_slots.length; ++i) {
    VkrViewUiTextSlot *slot = &state->text_slots.data[i];
    if (!slot->active) {
      *out_text_id = (uint32_t)i;
      *out_slot = slot;
      return true_v;
    }
  }

  log_error("UI text slots exhausted (max %llu)",
            (unsigned long long)state->text_slots.length);
  return false_v;
}

vkr_internal VkrViewUiTextSlot *
vkr_view_ui_get_active_slot(VkrViewUIState *state, uint32_t text_id) {
  if (!state || !state->text_slots.data ||
      text_id >= state->text_slots.length) {
    return NULL;
  }

  VkrViewUiTextSlot *slot = &state->text_slots.data[text_id];
  return slot->active ? slot : NULL;
}

vkr_internal void vkr_view_ui_position_slot(VkrViewUiTextSlot *slot,
                                            uint32_t width, uint32_t height) {
  if (!slot || !slot->active || width == 0 || height == 0) {
    return;
  }

  VkrTextBounds bounds = vkr_ui_text_get_bounds(&slot->text);
  float32_t x = slot->padding.x;
  float32_t y = slot->padding.y;

  switch (slot->anchor) {
  case VKR_VIEW_UI_TEXT_ANCHOR_TOP_RIGHT:
    x = (float32_t)width - bounds.size.x - slot->padding.x;
    y = (float32_t)height - bounds.size.y - slot->padding.y;
    break;
  case VKR_VIEW_UI_TEXT_ANCHOR_BOTTOM_LEFT:
    y = slot->padding.y;
    break;
  case VKR_VIEW_UI_TEXT_ANCHOR_BOTTOM_RIGHT:
    x = (float32_t)width - bounds.size.x - slot->padding.x;
    y = slot->padding.y;
    break;
  case VKR_VIEW_UI_TEXT_ANCHOR_TOP_LEFT:
  default:
    y = (float32_t)height - bounds.size.y - slot->padding.y;
    break;
  }

  if (x < 0.0f) {
    x = 0.0f;
  }
  if (y < 0.0f) {
    y = 0.0f;
  }

  vkr_ui_text_set_position(&slot->text, vec2_new(x, y));
}

vkr_internal bool8_t vkr_view_ui_get_screen_size(VkrLayerContext *ctx,
                                                 VkrViewUIState *state,
                                                 uint32_t *out_width,
                                                 uint32_t *out_height) {
  if (!state || !out_width || !out_height) {
    return false_v;
  }

  uint32_t width = state->screen_width;
  uint32_t height = state->screen_height;
  if (state->offscreen_enabled && state->offscreen_width > 0 &&
      state->offscreen_height > 0) {
    width = state->offscreen_width;
    height = state->offscreen_height;
  }

  if (width == 0 || height == 0) {
    width = vkr_layer_context_get_width(ctx);
    height = vkr_layer_context_get_height(ctx);
  }

  if ((width == 0 || height == 0) && ctx) {
    RendererFrontend *rf =
        (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
    if (rf) {
      width = rf->last_window_width;
      height = rf->last_window_height;
    }
  }

  if (width > 0 && height > 0) {
    if (!(state->offscreen_enabled && state->offscreen_width > 0 &&
          state->offscreen_height > 0)) {
      state->screen_width = width;
      state->screen_height = height;
    }
    *out_width = width;
    *out_height = height;
    return true_v;
  }

  *out_width = width;
  *out_height = height;
  return false_v;
}

vkr_internal void vkr_view_ui_destroy_offscreen_targets(RendererFrontend *rf,
                                                        VkrViewUIState *state) {
  if (!rf || !state) {
    return;
  }

  VkrRendererError wait_err = vkr_renderer_wait_idle(rf);
  if (wait_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(wait_err);
    log_warn("Wait idle failed before destroying UI offscreen targets: %s",
             string8_cstr(&err_str));
  }

  if (state->offscreen_targets) {
    for (uint32_t i = 0; i < state->offscreen_count; ++i) {
      if (state->offscreen_targets[i]) {
        vkr_renderer_render_target_destroy(rf, state->offscreen_targets[i]);
      }
    }
    vkr_allocator_free(&rf->allocator, state->offscreen_targets,
                       sizeof(VkrRenderTargetHandle) *
                           (uint64_t)state->offscreen_count,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  state->offscreen_targets = NULL;
  state->offscreen_colors = NULL;
  state->offscreen_color_layouts = NULL;
  state->offscreen_count = 0;
}

vkr_internal bool8_t vkr_view_ui_create_offscreen_targets(
    RendererFrontend *rf, VkrViewUIState *state, VkrTextureOpaqueHandle *colors,
    uint32_t count) {
  if (!rf || !state || !colors || count == 0) {
    return false_v;
  }

  vkr_view_ui_destroy_offscreen_targets(rf, state);

  state->offscreen_targets = (VkrRenderTargetHandle *)vkr_allocator_alloc(
      &rf->allocator, sizeof(VkrRenderTargetHandle) * (uint64_t)count,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!state->offscreen_targets) {
    log_error("Failed to allocate offscreen UI render targets");
    return false_v;
  }
  MemZero(state->offscreen_targets,
          sizeof(VkrRenderTargetHandle) * (uint64_t)count);

  uint32_t width =
      state->offscreen_width
          ? state->offscreen_width
          : (state->screen_width ? state->screen_width : rf->last_window_width);
  uint32_t height = state->offscreen_height
                        ? state->offscreen_height
                        : (state->screen_height ? state->screen_height
                                                : rf->last_window_height);
  if (width == 0 || height == 0) {
    width = rf->window ? rf->window->width : 0;
    height = rf->window ? rf->window->height : 0;
  }

  for (uint32_t i = 0; i < count; ++i) {
    VkrRenderTargetAttachmentRef attachments[1] = {
        {.texture = colors[i], .mip_level = 0, .base_layer = 0, .layer_count = 1},
    };
    VkrRenderTargetDesc rt_desc = {
        .sync_to_window_size = false_v,
        .attachment_count = 1,
        .attachments = attachments,
        .width = width,
        .height = height,
    };
    VkrRendererError rt_err = VKR_RENDERER_ERROR_NONE;
    state->offscreen_targets[i] = vkr_renderer_render_target_create(
        rf, &rt_desc, state->offscreen_renderpass, &rt_err);
    if (!state->offscreen_targets[i]) {
      String8 err = vkr_renderer_get_error_string(rt_err);
      log_error("Failed to create offscreen UI render target %u", i);
      log_error("Render target error: %s", string8_cstr(&err));
    }
  }

  state->offscreen_count = count;
  state->offscreen_colors = colors;
  return true_v;
}

vkr_internal bool32_t vkr_view_ui_on_create(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return false_v;
  }

  VkrViewUIState *state =
      (VkrViewUIState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return false_v;
  }

  VkrResourceHandleInfo ui_cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.ui.shadercfg"),
          &rf->scratch_allocator, &ui_cfg_info, &shadercfg_err)) {
    state->shader_config = *(VkrShaderConfig *)ui_cfg_info.as.custom;
  } else {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("UI shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  vkr_shader_system_create(&rf->shader_system, &state->shader_config);

  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &state->shader_config, VKR_PIPELINE_DOMAIN_UI,
          string8_lit("ui"), &state->pipeline, &pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(pipeline_error);
    log_error("Config UI pipeline failed: %s", string8_cstr(&err_str));
    return false_v;
  }
  if (state->shader_config.name.str && state->shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, state->pipeline, state->shader_config.name,
        &alias_err);
  }

  VkrResourceHandleInfo default_ui_material_info = {0};
  VkrRendererError material_load_error = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load(
          VKR_RESOURCE_TYPE_MATERIAL,
          string8_lit("assets/materials/default.ui.mt"), &rf->scratch_allocator,
          &default_ui_material_info, &material_load_error)) {
    state->material = default_ui_material_info.as.material;
  } else {
    String8 error_string = vkr_renderer_get_error_string(material_load_error);
    log_warn("Failed to load default UI material: %s",
             string8_cstr(&error_string));
  }

  VkrLayer *layer = ctx->layer;
  if (layer && !state->offscreen_renderpass) {
    VkrTextureFormat color_format = vkr_view_ui_get_swapchain_format(rf);
    VkrClearValue clear_ui = {.color_f32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    VkrRenderPassAttachmentDesc ui_color = {
        .format = color_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_LOAD,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .final_layout = VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .clear_value = clear_ui,
    };
    VkrRenderPassDesc ui_desc = {
        .name = string8_lit(VKR_VIEW_OFFSCREEN_UI_PASS_NAME),
        .domain = VKR_PIPELINE_DOMAIN_UI,
        .color_attachment_count = 1,
        .color_attachments = &ui_color,
        .depth_stencil_attachment = NULL,
        .resolve_attachment_count = 0,
        .resolve_attachments = NULL,
    };
    VkrRendererError pass_err = VKR_RENDERER_ERROR_NONE;
    state->offscreen_renderpass =
        vkr_renderer_renderpass_create_desc(rf, &ui_desc, &pass_err);
    if (!state->offscreen_renderpass) {
      String8 err = vkr_renderer_get_error_string(pass_err);
      log_error("Failed to create offscreen UI renderpass");
      log_error("Renderpass error: %s", string8_cstr(&err));
      return false_v;
    }
  }

  VkrRendererError ui_ls_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, state->pipeline, &state->instance_state,
          &ui_ls_err)) {
    String8 err_str = vkr_renderer_get_error_string(ui_ls_err);
    log_error("Failed to acquire local renderer state for UI pipeline: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  VkrResourceHandleInfo text_cfg_info = {0};
  VkrRendererError text_shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.text.shadercfg"),
          &rf->scratch_allocator, &text_cfg_info, &text_shadercfg_err)) {
    String8 err = vkr_renderer_get_error_string(text_shadercfg_err);
    log_error("Text shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  state->text_shader_config = *(VkrShaderConfig *)text_cfg_info.as.custom;
  vkr_shader_system_create(&rf->shader_system, &state->text_shader_config);

  VkrRendererError text_pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &state->text_shader_config,
          VKR_PIPELINE_DOMAIN_UI, string8_lit("ui_text"), &state->text_pipeline,
          &text_pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(text_pipeline_error);
    log_error("Config text pipeline failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  if (state->offscreen_renderpass) {
    VkrShaderConfig offscreen_text_cfg = state->text_shader_config;
    offscreen_text_cfg.renderpass_name =
        string8_lit(VKR_VIEW_OFFSCREEN_UI_PASS_NAME);
    offscreen_text_cfg.name = (String8){0};

    VkrRendererError offscreen_text_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_create_from_shader_config(
            &rf->pipeline_registry, &offscreen_text_cfg, VKR_PIPELINE_DOMAIN_UI,
            string8_lit("ui_text_offscreen"), &state->text_pipeline_offscreen,
            &offscreen_text_err)) {
      String8 err_str = vkr_renderer_get_error_string(offscreen_text_err);
      log_error("Config offscreen text pipeline failed: %s",
                string8_cstr(&err_str));
      return false_v;
    }
  }

  if (state->text_shader_config.name.str &&
      state->text_shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, state->text_pipeline,
        state->text_shader_config.name, &alias_err);
  }

  state->text_slots =
      array_create_VkrViewUiTextSlot(&rf->allocator, VKR_VIEW_UI_MAX_TEXTS);
  MemZero(state->text_slots.data,
          sizeof(VkrViewUiTextSlot) * (uint64_t)state->text_slots.length);
  state->screen_width = vkr_layer_context_get_width(ctx);
  state->screen_height = vkr_layer_context_get_height(ctx);

  return true_v;
}

vkr_internal void vkr_view_ui_on_attach(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  vkr_view_ui_on_resize(ctx, vkr_layer_context_get_width(ctx),
                        vkr_layer_context_get_height(ctx));
}

vkr_internal void vkr_view_ui_on_resize(VkrLayerContext *ctx, uint32_t width,
                                        uint32_t height) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  rf->globals.ui_view = mat4_identity();
  rf->globals.ui_projection =
      mat4_ortho(0.0f, (float32_t)width, (float32_t)height, 0.0f, -1.0f, 1.0f);

  vkr_layer_context_set_camera(ctx, &rf->globals.ui_view,
                               &rf->globals.ui_projection);

  VkrViewUIState *state =
      (VkrViewUIState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  if (state->offscreen_enabled && state->offscreen_width > 0 &&
      state->offscreen_height > 0) {
    state->screen_width = state->offscreen_width;
    state->screen_height = state->offscreen_height;
  } else {
    state->screen_width = width;
    state->screen_height = height;
  }

  if (state->offscreen_enabled && state->offscreen_colors &&
      state->offscreen_color_layouts && state->offscreen_count > 0) {
    vkr_view_ui_set_offscreen_enabled(
        rf, true_v, state->offscreen_colors, state->offscreen_color_layouts,
        state->offscreen_count, state->offscreen_width,
        state->offscreen_height);
  }

  uint32_t layout_width = state->offscreen_enabled && state->offscreen_width > 0
                              ? state->offscreen_width
                              : width;
  uint32_t layout_height =
      state->offscreen_enabled && state->offscreen_height > 0
          ? state->offscreen_height
          : height;

  for (uint64_t i = 0; i < state->text_slots.length; ++i) {
    VkrViewUiTextSlot *slot = &state->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_view_ui_position_slot(slot, layout_width, layout_height);
  }
}

vkr_internal void vkr_view_ui_on_render(VkrLayerContext *ctx,
                                        const VkrLayerRenderInfo *info) {
  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return;
  }

  VkrViewUIState *state =
      (VkrViewUIState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  (void)info;

  bool8_t override_projection = state->offscreen_enabled &&
                                state->offscreen_width > 0 &&
                                state->offscreen_height > 0;
  Mat4 previous_view = rf->globals.ui_view;
  Mat4 previous_projection = rf->globals.ui_projection;
  if (override_projection) {
    rf->globals.ui_view = mat4_identity();
    rf->globals.ui_projection =
        mat4_ortho(0.0f, (float32_t)state->offscreen_width,
                   (float32_t)state->offscreen_height, 0.0f, -1.0f, 1.0f);
  }

  for (uint64_t i = 0; i < state->text_slots.length; ++i) {
    VkrViewUiTextSlot *slot = &state->text_slots.data[i];
    if (!slot->active) {
      continue;
    }
    vkr_ui_text_draw(&slot->text);
  }

  if (override_projection) {
    rf->globals.ui_view = previous_view;
    rf->globals.ui_projection = previous_projection;
  }
}

vkr_internal void vkr_view_ui_on_data_received(VkrLayerContext *ctx,
                                               const VkrLayerMsgHeader *msg,
                                               void *out_rsp,
                                               uint64_t out_rsp_capacity,
                                               uint64_t *out_rsp_size) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(msg != NULL, "Message is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  VkrViewUIState *state =
      (VkrViewUIState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  uint32_t width = 0;
  uint32_t height = 0;

  switch (msg->kind) {
  case VKR_LAYER_MSG_UI_TEXT_CREATE: {
    const VkrViewUiTextCreateData *payload =
        (const VkrViewUiTextCreateData *)((const uint8_t *)msg +
                                          sizeof(VkrLayerMsgHeader));
    VkrPipelineHandle text_pipeline = state->text_pipeline;
    if (state->offscreen_enabled && state->text_pipeline_offscreen.id != 0) {
      text_pipeline = state->text_pipeline_offscreen;
    }

    if (text_pipeline.id == 0) {
      log_error("UI text pipeline not ready");
      return;
    }

    uint32_t text_id = payload->text_id;
    VkrViewUiTextSlot *slot = NULL;
    if (text_id == VKR_INVALID_ID) {
      if (!vkr_view_ui_find_free_slot(state, &text_id, &slot)) {
        return;
      }
    } else {
      if (!vkr_view_ui_ensure_slot(state, text_id, &slot)) {
        return;
      }
    }

    if (slot->active) {
      log_warn("UI text id %u already active; replacing", text_id);
      vkr_ui_text_destroy(&slot->text);
      slot->active = false_v;
    }

    VkrRendererError text_err = VKR_RENDERER_ERROR_NONE;
    const VkrUiTextConfig *config =
        payload->has_config ? &payload->config : NULL;
    if (!vkr_ui_text_create(rf, &rf->allocator, &rf->font_system, text_pipeline,
                            payload->content, config, &slot->text, &text_err)) {
      String8 err = vkr_renderer_get_error_string(text_err);
      log_error("Failed to create UI text: %s", string8_cstr(&err));
      return;
    }

    slot->active = true_v;
    slot->anchor = payload->anchor;
    slot->padding = payload->padding;

    vkr_view_ui_get_screen_size(ctx, state, &width, &height);
    vkr_view_ui_position_slot(slot, width, height);

    // Fill typed response
    if (out_rsp && out_rsp_capacity >= sizeof(VkrLayerRsp_UiTextCreate)) {
      VkrLayerRsp_UiTextCreate *rsp = (VkrLayerRsp_UiTextCreate *)out_rsp;
      rsp->h.kind = VKR_LAYER_RSP_UI_TEXT_CREATE;
      rsp->h.version = 1;
      rsp->h.data_size = sizeof(uint32_t);
      rsp->h.error = 0;
      rsp->text_id = text_id;
      if (out_rsp_size) {
        *out_rsp_size = sizeof(VkrLayerRsp_UiTextCreate);
      }
    }
  } break;
  case VKR_LAYER_MSG_UI_TEXT_UPDATE: {
    const VkrViewUiTextUpdateData *payload =
        (const VkrViewUiTextUpdateData *)((const uint8_t *)msg +
                                          sizeof(VkrLayerMsgHeader));
    VkrViewUiTextSlot *slot =
        vkr_view_ui_get_active_slot(state, payload->text_id);
    if (!slot) {
      log_warn("UI text id %u not found for update", payload->text_id);
      return;
    }

    if (!vkr_ui_text_set_content(&slot->text, payload->content)) {
      log_error("Failed to update UI text content");
      return;
    }

    vkr_view_ui_get_screen_size(ctx, state, &width, &height);
    vkr_view_ui_position_slot(slot, width, height);
  } break;
  case VKR_LAYER_MSG_UI_TEXT_DESTROY: {
    const VkrViewUiTextDestroyData *payload =
        (const VkrViewUiTextDestroyData *)((const uint8_t *)msg +
                                           sizeof(VkrLayerMsgHeader));
    VkrViewUiTextSlot *slot =
        vkr_view_ui_get_active_slot(state, payload->text_id);
    if (!slot) {
      log_warn("UI text id %u not found for destroy", payload->text_id);
      return;
    }

    vkr_ui_text_destroy(&slot->text);
    slot->active = false_v;
  } break;
  default:
    log_warn("UI view received unsupported message kind %u", msg->kind);
    break;
  }
}

vkr_internal void vkr_view_ui_on_detach(VkrLayerContext *ctx) {
  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return;
  }

  (void)rf;
}

vkr_internal void vkr_view_ui_on_destroy(VkrLayerContext *ctx) {
  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return;
  }

  VkrViewUIState *state =
      (VkrViewUIState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  if (state->instance_state.id != 0 && state->pipeline.id != 0) {
    vkr_pipeline_registry_release_instance_state(
        &rf->pipeline_registry, state->pipeline, state->instance_state,
        &(VkrRendererError){0});
  }

  for (uint64_t i = 0; i < state->text_slots.length; ++i) {
    VkrViewUiTextSlot *slot = &state->text_slots.data[i];
    if (slot->active) {
      vkr_ui_text_destroy(&slot->text);
      slot->active = false_v;
    }
  }
  array_destroy_VkrViewUiTextSlot(&state->text_slots);

  if (state->text_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           state->text_pipeline);
  }

  if (state->pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           state->pipeline);
  }

  if (state->text_pipeline_offscreen.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           state->text_pipeline_offscreen);
  }

  vkr_view_ui_destroy_offscreen_targets(rf, state);

  if (state->offscreen_renderpass) {
    vkr_renderer_renderpass_destroy(rf, state->offscreen_renderpass);
    state->offscreen_renderpass = NULL;
  }
}

vkr_internal void vkr_view_ui_rebuild_texts(RendererFrontend *rf,
                                            VkrViewUIState *state,
                                            VkrPipelineHandle pipeline) {
  if (!rf || !state || pipeline.id == 0) {
    return;
  }

  uint32_t width =
      state->screen_width ? state->screen_width : rf->last_window_width;
  uint32_t height =
      state->screen_height ? state->screen_height : rf->last_window_height;

  for (uint64_t i = 0; i < state->text_slots.length; ++i) {
    VkrViewUiTextSlot *slot = &state->text_slots.data[i];
    if (!slot->active) {
      continue;
    }

    VkrUiText new_text = {0};
    VkrRendererError text_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_ui_text_create(rf, &rf->allocator, &rf->font_system, pipeline,
                            slot->text.content, &slot->text.config, &new_text,
                            &text_err)) {
      String8 err = vkr_renderer_get_error_string(text_err);
      log_error("Failed to rebuild UI text pipeline: %s", string8_cstr(&err));
      continue;
    }

    vkr_ui_text_destroy(&slot->text);
    slot->text = new_text;
    vkr_view_ui_position_slot(slot, width, height);
  }
}

vkr_internal VkrLayer *vkr_view_ui_find_layer(VkrViewSystem *vs,
                                              VkrLayerHandle handle) {
  if (!vs || !vs->initialized || handle.id == 0) {
    return NULL;
  }

  if (handle.id - 1 >= vs->layers.length) {
    return NULL;
  }

  VkrLayer *layer = array_get_VkrLayer(&vs->layers, handle.id - 1);
  if (!layer->active) {
    return NULL;
  }

  if (layer->handle.generation != handle.generation) {
    return NULL;
  }

  return layer;
}
