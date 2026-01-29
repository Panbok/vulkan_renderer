/**
 * @file vkr_view_editor.c
 * @brief Editor viewport layer implementation.
 *
 * The editor layer provides an editor-style layout with UI panels surrounding
 * a central 3D viewport. It renders the offscreen scene texture to a viewport
 * plane within the editor UI. The layer is disabled by default and can be
 * toggled at runtime via the VKR_VIEW_WORLD_DATA_TOGGLE_OFFSCREEN message.
 *
 * Key features:
 * - Dynamic viewport computation based on window size
 * - Automatic offscreen target size synchronization with World layer
 * - Transform-based viewport plane positioning and scaling
 */

#include "renderer/systems/views/vkr_view_editor.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vkr_math.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/views/vkr_view_world.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_layer_messages.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_view_system.h"
#include "renderer/vulkan/vulkan_types.h"

/**
 * @brief Internal state for the editor viewport layer.
 *
 * Manages the viewport display pipeline, material, geometry, and layout
 * calculations for rendering the offscreen scene into the editor UI.
 */
typedef struct VkrViewEditorState {
  VkrShaderConfig shader_config; /**< Shader config for viewport display. */
  VkrPipelineHandle pipeline;    /**< Pipeline for rendering viewport quad. */
  VkrMaterialHandle material; /**< Material with offscreen texture binding. */
  VkrRendererInstanceStateHandle
      instance_state;                    /**< Pipeline instance state. */
  VkrRenderPassHandle editor_renderpass; /**< Render pass for editor layer. */
  VkrTransform transform; /**< Transform for viewport quad positioning. */
  VkrGeometryHandle
      viewport_geometry;    /**< Quad geometry for viewport display. */
  Vec2 viewport_plane_size; /**< Base size of the viewport plane (2x2). */
  Vec4 viewport_rect;       /**< Viewport panel rect (x, y, w, h) in pixels. */
  VkrViewportMapping
      viewport_mapping; /**< Panel->image mapping used for picking/mapping. */
  VkrViewportFitMode fit_mode; /**< How to fit scene texture in panel. */
  float32_t render_scale;      /**< Render target scale relative to panel. */
  uint32_t screen_width;       /**< Current screen width in pixels. */
  uint32_t screen_height;      /**< Current screen height in pixels. */
  uint32_t last_notified_offscreen_width;  /**< Last sent target width. */
  uint32_t last_notified_offscreen_height; /**< Last sent target height. */
} VkrViewEditorState;

vkr_internal VkrTextureFormat vkr_view_editor_get_swapchain_format(
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

/* Layer lifecycle callbacks */
vkr_internal bool32_t vkr_view_editor_on_create(VkrLayerContext *ctx);
vkr_internal void vkr_view_editor_on_attach(VkrLayerContext *ctx);
vkr_internal void vkr_view_editor_on_enable(VkrLayerContext *ctx);
vkr_internal void vkr_view_editor_on_resize(VkrLayerContext *ctx,
                                            uint32_t width, uint32_t height);
vkr_internal void vkr_view_editor_on_render(VkrLayerContext *ctx,
                                            const VkrLayerRenderInfo *info);
vkr_internal void vkr_view_editor_on_data_received(VkrLayerContext *ctx,
                                                   const VkrLayerMsgHeader *msg,
                                                   void *out_rsp,
                                                   uint64_t out_rsp_capacity,
                                                   uint64_t *out_rsp_size);
vkr_internal void vkr_view_editor_on_destroy(VkrLayerContext *ctx);

/* Internal helpers */
vkr_internal void
vkr_view_editor_notify_offscreen_size(RendererFrontend *rf,
                                      VkrViewEditorState *state);
vkr_internal void
vkr_view_editor_update_viewport_mapping(VkrViewEditorState *state);

/**
 * @brief Computes the viewport rectangle for the central 3D scene area.
 *
 * Calculates the viewport bounds based on editor panel sizes:
 * - Top bar: 6% of height (min 32px)
 * - Bottom panel: 24% of height (min 180px)
 * - Left panel: 18% of width (min 220px)
 * - Right panel: 22% of width (min 280px)
 * - Gutter: 8px padding around viewport
 *
 * @param width  Screen width in pixels
 * @param height Screen height in pixels
 * @return Vec4 containing (x, y, width, height) of the viewport area
 */
vkr_internal Vec4 vkr_view_editor_compute_viewport(uint32_t width,
                                                   uint32_t height) {
  // NOTE: Pixel alignment matters for picking and crisp sampling. Compute an
  // integer pixel-aligned rect (stored as floats for convenience).
  const uint32_t top_bar =
      (uint32_t)vkr_max_f32(32.0f, vkr_round_f32((float32_t)height * 0.06f));
  const uint32_t bottom_panel =
      (uint32_t)vkr_max_f32(180.0f, vkr_round_f32((float32_t)height * 0.24f));
  const uint32_t left_panel =
      (uint32_t)vkr_max_f32(220.0f, vkr_round_f32((float32_t)width * 0.18f));
  const uint32_t right_panel =
      (uint32_t)vkr_max_f32(280.0f, vkr_round_f32((float32_t)width * 0.22f));
  const uint32_t gutter = 8u;

  const uint32_t x = left_panel + gutter;
  const uint32_t y = top_bar + gutter;

  const uint32_t used_w = left_panel + right_panel + gutter * 2u;
  const uint32_t used_h = top_bar + bottom_panel + gutter * 2u;

  const uint32_t w = (width > used_w) ? (width - used_w) : 1u;
  const uint32_t h = (height > used_h) ? (height - used_h) : 1u;

  return (Vec4){(float32_t)x, (float32_t)y, (float32_t)w, (float32_t)h};
}

/**
 * @brief Updates the viewport plane transform based on current viewport rect.
 *
 * Scales the base 2x2 viewport plane to match the computed viewport rectangle
 * and positions it at the correct screen coordinates for the editor layout.
 *
 * @param state Editor view state containing viewport geometry and rect
 */
vkr_internal void
vkr_view_editor_update_viewport_transform(VkrViewEditorState *state) {
  if (!state || state->viewport_plane_size.x <= 0.0f ||
      state->viewport_plane_size.y <= 0.0f) {
    return;
  }

  Vec4 rect = state->viewport_mapping.image_rect_px;
  if (rect.z <= 0.0f || rect.w <= 0.0f) {
    rect = state->viewport_rect;
  }
  float32_t scale_x = rect.z / state->viewport_plane_size.x;
  float32_t scale_y = rect.w / state->viewport_plane_size.y;
  state->transform = vkr_transform_from_position_scale_rotation(
      vec3_new(rect.x, rect.y, 0.0f), vec3_new(scale_x, scale_y, 1.0f),
      vkr_quat_identity());
}

vkr_internal void
vkr_view_editor_update_viewport_mapping(VkrViewEditorState *state) {
  if (!state) {
    return;
  }

  // Clamp render scale to keep resource usage reasonable (Phase 4 hardening).
  state->render_scale = vkr_clamp_f32(state->render_scale, 0.25f, 2.0f);

  const Vec4 panel = state->viewport_rect;
  const float32_t panel_w = vkr_max_f32(1.0f, panel.z);
  const float32_t panel_h = vkr_max_f32(1.0f, panel.w);

  uint32_t target_w =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel_w * state->render_scale));
  uint32_t target_h =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(panel_h * state->render_scale));

  Vec4 image = panel;

  if (state->fit_mode == VKR_VIEWPORT_FIT_CONTAIN) {
    const float32_t target_aspect = (float32_t)target_w / (float32_t)target_h;
    const float32_t panel_aspect = panel_w / panel_h;

    if (target_aspect > panel_aspect) {
      // Fit to width, letterbox vertically.
      const float32_t scale = panel_w / (float32_t)target_w;
      const float32_t img_h = vkr_max_f32(1.0f, (float32_t)target_h * scale);
      const float32_t y = panel.y + (panel_h - img_h) * 0.5f;
      image = (Vec4){panel.x, y, panel_w, img_h};
    } else if (target_aspect < panel_aspect) {
      // Fit to height, pillarbox horizontally.
      const float32_t scale = panel_h / (float32_t)target_h;
      const float32_t img_w = vkr_max_f32(1.0f, (float32_t)target_w * scale);
      const float32_t x = panel.x + (panel_w - img_w) * 0.5f;
      image = (Vec4){x, panel.y, img_w, panel_h};
    } else {
      image = panel;
    }

    // Snap to pixel boundaries for stable mapping.
    image.x = vkr_round_f32(image.x);
    image.y = vkr_round_f32(image.y);
    image.z = vkr_max_f32(1.0f, vkr_round_f32(image.z));
    image.w = vkr_max_f32(1.0f, vkr_round_f32(image.w));
  }

  state->viewport_mapping = (VkrViewportMapping){
      .panel_rect_px = panel,
      .image_rect_px = image,
      .target_width = target_w,
      .target_height = target_h,
      .fit_mode = state->fit_mode,
  };
}

bool32_t vkr_view_editor_register(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (!rf->view_system.initialized) {
    log_error("View system not initialized; cannot register editor view");
    return false_v;
  }

  if (rf->editor_layer.id != 0) {
    return true_v;
  }

  VkrLayerPassConfig editor_passes[1] = {{
      .renderpass_name = string8_lit("Renderpass.Editor"),
      .use_swapchain_color = true_v,
      .use_depth = false_v,
  }};

  VkrViewEditorState *state =
      vkr_allocator_alloc(&rf->allocator, sizeof(VkrViewEditorState),
                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!state) {
    log_error("Failed to allocate editor view state");
    return false_v;
  }
  MemZero(state, sizeof(*state));
  state->transform = vkr_transform_identity();
  state->pipeline = VKR_PIPELINE_HANDLE_INVALID;
  state->editor_renderpass = NULL;
  state->fit_mode = VKR_VIEWPORT_FIT_STRETCH;
  state->render_scale = 1.0f;
  state->viewport_rect = (Vec4){0.0f, 0.0f, 1.0f, 1.0f};
  vkr_view_editor_update_viewport_mapping(state);

  VkrLayerConfig editor_cfg = {
      .name = string8_lit("Layer.Editor"),
      .order = 2,
      .width = 0,
      .height = 0,
      .view = rf->globals.ui_view,
      .projection = rf->globals.ui_projection,
      .pass_count = ArrayCount(editor_passes),
      .passes = editor_passes,
      .callbacks = {.on_create = vkr_view_editor_on_create,
                    .on_attach = vkr_view_editor_on_attach,
                    .on_enable = vkr_view_editor_on_enable,
                    .on_resize = vkr_view_editor_on_resize,
                    .on_render = vkr_view_editor_on_render,
                    .on_data_received = vkr_view_editor_on_data_received,
                    .on_destroy = vkr_view_editor_on_destroy},
      .user_data = state,
      .enabled = false_v,
  };

  VkrRendererError layer_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_view_system_register_layer(rf, &editor_cfg, &rf->editor_layer,
                                      &layer_err)) {
    String8 err = vkr_renderer_get_error_string(layer_err);
    log_error("Failed to register editor view: %s", string8_cstr(&err));
    return false_v;
  }

  vkr_view_system_set_layer_enabled(rf, rf->editor_layer, false_v);

  return true_v;
}

vkr_internal bool32_t vkr_view_editor_on_create(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return false_v;
  }

  VkrViewEditorState *state =
      (VkrViewEditorState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return false_v;
  }

  if (!state->editor_renderpass) {
    VkrRenderPassHandle existing =
        vkr_renderer_renderpass_get(rf, string8_lit("Renderpass.Editor"));
    if (existing) {
      state->editor_renderpass = existing;
    } else {
      VkrTextureFormat color_format = vkr_view_editor_get_swapchain_format(rf);
      VkrClearValue clear_color = {.color_f32 = {0.0f, 0.0f, 0.0f, 1.0f}};
      VkrRenderPassAttachmentDesc editor_color = {
          .format = color_format,
          .samples = VKR_SAMPLE_COUNT_1,
          .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
          .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
          .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
          .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
          .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
          .final_layout = VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR,
          .clear_value = clear_color,
      };
      VkrRenderPassDesc editor_desc = {
          .name = string8_lit("Renderpass.Editor"),
          .domain = VKR_PIPELINE_DOMAIN_UI,
          .color_attachment_count = 1,
          .color_attachments = &editor_color,
          .depth_stencil_attachment = NULL,
          .resolve_attachment_count = 0,
          .resolve_attachments = NULL,
      };
      VkrRendererError pass_err = VKR_RENDERER_ERROR_NONE;
      state->editor_renderpass =
          vkr_renderer_renderpass_create_desc(rf, &editor_desc, &pass_err);
      if (!state->editor_renderpass) {
        String8 err = vkr_renderer_get_error_string(pass_err);
        log_error("Failed to create editor renderpass");
        log_error("Renderpass error: %s", string8_cstr(&err));
        return false_v;
      }
    }
  }

  VkrResourceHandleInfo cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.viewport_display.shadercfg"),
          &rf->scratch_allocator, &cfg_info, &shadercfg_err)) {
    state->shader_config = *(VkrShaderConfig *)cfg_info.as.custom;
  } else {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("Editor viewport shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  vkr_shader_system_create(&rf->shader_system, &state->shader_config);

  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &state->shader_config, VKR_PIPELINE_DOMAIN_UI,
          string8_lit("editor_viewport"), &state->pipeline, &pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(pipeline_error);
    log_error("Config editor viewport pipeline failed: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  if (state->shader_config.name.str && state->shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, state->pipeline, state->shader_config.name,
        &alias_err);
  }

  VkrResourceHandleInfo material_info = {0};
  VkrRendererError material_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load(
          VKR_RESOURCE_TYPE_MATERIAL,
          string8_lit("assets/materials/default.viewport_display.mt"),
          &rf->scratch_allocator, &material_info, &material_err)) {
    state->material = material_info.as.material;
  } else {
    String8 err = vkr_renderer_get_error_string(material_err);
    log_warn("Editor viewport material load failed: %s", string8_cstr(&err));
  }

  VkrRendererError geo_err = VKR_RENDERER_ERROR_NONE;
  VkrVertex2d verts[4] = {0};
  float32_t width = 2.0f;
  float32_t height = 2.0f;

  verts[0].position = vec2_new(0.0f, 0.0f);
  verts[0].texcoord = vec2_new(0.0f, 1.0f);

  verts[1].position = vec2_new(width, height);
  verts[1].texcoord = vec2_new(1.0f, 0.0f);

  verts[2].position = vec2_new(0.0f, height);
  verts[2].texcoord = vec2_new(0.0f, 0.0f);

  verts[3].position = vec2_new(width, 0.0f);
  verts[3].texcoord = vec2_new(1.0f, 1.0f);

  uint32_t indices[6] = {2, 1, 0, 3, 0, 1};

  VkrGeometryConfig geo_cfg = {0};
  geo_cfg.vertex_size = sizeof(VkrVertex2d);
  geo_cfg.vertex_count = 4;
  geo_cfg.vertices = verts;
  geo_cfg.index_size = sizeof(uint32_t);
  geo_cfg.index_count = 6;
  geo_cfg.indices = indices;
  geo_cfg.center = vec3_zero();
  geo_cfg.min_extents = vec3_new(-width, -height, 0.0f);
  geo_cfg.max_extents = vec3_new(width, height, 0.0f);
  string_format(geo_cfg.name, sizeof(geo_cfg.name), "Editor Viewport Plane");

  state->viewport_geometry = vkr_geometry_system_create(
      &rf->geometry_system, &geo_cfg, true_v, &geo_err);
  if (state->viewport_geometry.id == 0) {
    String8 err = vkr_renderer_get_error_string(geo_err);
    log_error("Failed to create editor viewport geometry: %s",
              string8_cstr(&err));
  }
  state->viewport_plane_size = vec2_new(width, height);

  VkrRendererError instance_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, state->pipeline, &state->instance_state,
          &instance_err)) {
    String8 err_str = vkr_renderer_get_error_string(instance_err);
    log_error("Failed to acquire instance state for editor viewport: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  return true_v;
}

vkr_internal void vkr_view_editor_on_attach(VkrLayerContext *ctx) {
  vkr_view_editor_on_resize(ctx, vkr_layer_context_get_width(ctx),
                            vkr_layer_context_get_height(ctx));
}

vkr_internal void vkr_view_editor_on_enable(VkrLayerContext *ctx) {
  vkr_view_editor_on_resize(ctx, vkr_layer_context_get_width(ctx),
                            vkr_layer_context_get_height(ctx));
}

vkr_internal void vkr_view_editor_on_resize(VkrLayerContext *ctx,
                                            uint32_t width, uint32_t height) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return;
  }

  rf->globals.ui_view = mat4_identity();
  rf->globals.ui_projection =
      mat4_ortho(0.0f, (float32_t)width, (float32_t)height, 0.0f, -1.0f, 1.0f);

  vkr_layer_context_set_camera(ctx, &rf->globals.ui_view,
                               &rf->globals.ui_projection);

  VkrViewEditorState *state =
      (VkrViewEditorState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  state->screen_width = width;
  state->screen_height = height;
  state->viewport_rect = vkr_view_editor_compute_viewport(width, height);
  vkr_view_editor_update_viewport_mapping(state);
  vkr_view_editor_update_viewport_transform(state);
  vkr_view_editor_notify_offscreen_size(rf, state);
}

vkr_internal void vkr_view_editor_on_render(VkrLayerContext *ctx,
                                            const VkrLayerRenderInfo *info) {
  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf || !info) {
    return;
  }

  VkrViewEditorState *state =
      (VkrViewEditorState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  if (!rf->offscreen_color_handles ||
      info->image_index >= rf->offscreen_color_handle_count) {
    return;
  }

  VkrMaterial *viewport_material =
      vkr_material_system_get_by_handle(&rf->material_system, state->material);
  if (!viewport_material) {
    return;
  }

  viewport_material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
      rf->offscreen_color_handles[info->image_index];
  viewport_material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true_v;

  const char *shader_name =
      (viewport_material->shader_name && viewport_material->shader_name[0])
          ? viewport_material->shader_name
          : "shader.default.viewport_display";
  if (!vkr_shader_system_use(&rf->shader_system, shader_name)) {
    shader_name = "shader.default.ui";
    vkr_shader_system_use(&rf->shader_system, shader_name);
  }

  uint32_t pipeline_id =
      viewport_material ? viewport_material->pipeline_id : VKR_INVALID_ID;
  VkrPipelineHandle resolved = VKR_PIPELINE_HANDLE_INVALID;
  VkrRendererError get_err = VKR_RENDERER_ERROR_NONE;
  vkr_pipeline_registry_get_pipeline_for_material(
      &rf->pipeline_registry, shader_name, pipeline_id, &resolved, &get_err);

  if (state->pipeline.id != resolved.id ||
      state->pipeline.generation != resolved.generation) {
    if (state->pipeline.id != 0) {
      vkr_pipeline_registry_release_instance_state(
          &rf->pipeline_registry, state->pipeline, state->instance_state,
          &(VkrRendererError){0});
    }
    VkrRendererError acq_err = VKR_RENDERER_ERROR_NONE;
    if (vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, resolved, &state->instance_state,
            &acq_err)) {
      state->pipeline = resolved;
    } else {
      String8 err_str = vkr_renderer_get_error_string(acq_err);
      log_error("Failed to acquire editor viewport instance state: %s",
                string8_cstr(&err_str));
      return;
    }
  }

  VkrPipelineHandle current_pipeline =
      vkr_pipeline_registry_get_current_pipeline(&rf->pipeline_registry);
  if (current_pipeline.id != resolved.id ||
      current_pipeline.generation != resolved.generation) {
    VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, resolved,
                                             &bind_err)) {
      String8 err_str = vkr_renderer_get_error_string(bind_err);
      log_error("Failed to bind editor viewport pipeline: %s",
                string8_cstr(&err_str));
      return;
    }
  }

  rf->draw_state.instance_state = state->instance_state;

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_UI);
  vkr_material_system_apply_local(
      &rf->material_system,
      &(VkrLocalMaterialState){.model =
                                   vkr_transform_get_world(&state->transform)});

  vkr_shader_system_bind_instance(&rf->shader_system, state->instance_state.id);
  vkr_material_system_apply_instance(&rf->material_system, viewport_material,
                                     VKR_PIPELINE_DOMAIN_UI);

  VkrGeometryHandle plane =
      (state->viewport_geometry.id != 0)
          ? state->viewport_geometry
          : vkr_geometry_system_get_default_plane2d(&rf->geometry_system);
  vkr_geometry_system_render(rf, &rf->geometry_system, plane, 1);
}

vkr_internal void vkr_view_editor_on_data_received(VkrLayerContext *ctx,
                                                   const VkrLayerMsgHeader *msg,
                                                   void *out_rsp,
                                                   uint64_t out_rsp_capacity,
                                                   uint64_t *out_rsp_size) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(msg != NULL, "Message is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return;
  }

  VkrViewEditorState *state =
      (VkrViewEditorState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  if (out_rsp_size) {
    *out_rsp_size = 0;
  }

  switch (msg->kind) {
  case VKR_LAYER_MSG_EDITOR_GET_VIEWPORT_MAPPING: {
    if (out_rsp &&
        out_rsp_capacity >= sizeof(VkrLayerRsp_EditorViewportMapping)) {
      VkrLayerRsp_EditorViewportMapping *rsp =
          (VkrLayerRsp_EditorViewportMapping *)out_rsp;
      rsp->h.kind = VKR_LAYER_RSP_EDITOR_VIEWPORT_MAPPING;
      rsp->h.version = 1;
      rsp->h.data_size = sizeof(VkrViewportMapping);
      rsp->h.error = 0;
      rsp->mapping = state->viewport_mapping;
      if (out_rsp_size) {
        *out_rsp_size = sizeof(VkrLayerRsp_EditorViewportMapping);
      }
    }
  } break;

  case VKR_LAYER_MSG_EDITOR_SET_VIEWPORT_FIT_MODE: {
    const VkrViewportFitMode *payload =
        (const VkrViewportFitMode *)((const uint8_t *)msg +
                                     sizeof(VkrLayerMsgHeader));
    VkrViewportFitMode mode = *payload;
    if (mode != VKR_VIEWPORT_FIT_STRETCH && mode != VKR_VIEWPORT_FIT_CONTAIN) {
      mode = VKR_VIEWPORT_FIT_STRETCH;
    }
    state->fit_mode = mode;

    vkr_view_editor_update_viewport_mapping(state);
    vkr_view_editor_update_viewport_transform(state);
    vkr_view_editor_notify_offscreen_size(rf, state);
  } break;

  case VKR_LAYER_MSG_EDITOR_SET_RENDER_SCALE: {
    const float32_t *payload =
        (const float32_t *)((const uint8_t *)msg + sizeof(VkrLayerMsgHeader));
    float32_t scale = *payload;
    // Handle NaN/negative/zero safely.
    if (!(scale > 0.0f)) {
      scale = 1.0f;
    }
    state->render_scale = scale;

    vkr_view_editor_update_viewport_mapping(state);
    vkr_view_editor_update_viewport_transform(state);
    vkr_view_editor_notify_offscreen_size(rf, state);
  } break;

  default:
    log_warn("Editor view received unsupported message kind %u", msg->kind);
    break;
  }
}

vkr_internal void vkr_view_editor_on_destroy(VkrLayerContext *ctx) {
  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return;
  }

  VkrViewEditorState *state =
      (VkrViewEditorState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  if (state->instance_state.id != 0 && state->pipeline.id != 0) {
    vkr_pipeline_registry_release_instance_state(
        &rf->pipeline_registry, state->pipeline, state->instance_state,
        &(VkrRendererError){0});
  }

  if (state->viewport_geometry.id != 0) {
    vkr_geometry_system_release(&rf->geometry_system, state->viewport_geometry);
    state->viewport_geometry = VKR_GEOMETRY_HANDLE_INVALID;
  }

  if (state->editor_renderpass) {
    vkr_renderer_renderpass_destroy(rf, state->editor_renderpass);
    state->editor_renderpass = NULL;
  }

  if (state->pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           state->pipeline);
  }
}

/**
 * @brief Notifies the World layer of the current offscreen target size.
 *
 * Sends the computed viewport dimensions to the World layer so it can
 * resize its offscreen render targets accordingly. Only sends the message
 * when the editor layer is enabled.
 *
 * @param rf    Renderer frontend
 * @param state Editor view state with current viewport rect
 */
vkr_internal void
vkr_view_editor_notify_offscreen_size(RendererFrontend *rf,
                                      VkrViewEditorState *state) {
  if (!rf || !state) {
    return;
  }

  if (!vkr_view_system_is_layer_enabled(rf, rf->editor_layer)) {
    return;
  }

  uint32_t width = state->viewport_mapping.target_width;
  uint32_t height = state->viewport_mapping.target_height;
  if (width == 0 || height == 0) {
    width = (uint32_t)vkr_max_f32(1.0f, state->viewport_rect.z);
    height = (uint32_t)vkr_max_f32(1.0f, state->viewport_rect.w);
  }

  if (state->last_notified_offscreen_width == width &&
      state->last_notified_offscreen_height == height) {
    return;
  }

  state->last_notified_offscreen_width = width;
  state->last_notified_offscreen_height = height;

  // Use typed message API for offscreen size notification
  VkrLayerMsg_WorldSetOffscreenSize msg = {
      .h = VKR_LAYER_MSG_HEADER_INIT(VKR_LAYER_MSG_WORLD_SET_OFFSCREEN_SIZE,
                                     VkrViewWorldOffscreenSizeData),
      .payload = {.width = width, .height = height},
  };
  vkr_view_system_send_msg_no_rsp(rf, rf->world_layer, &msg.h);
}

bool8_t vkr_view_editor_get_viewport_mapping(RendererFrontend *rf,
                                             VkrViewportMapping *out_mapping) {
  if (!rf || !out_mapping) {
    return false_v;
  }
  if (!rf->view_system.initialized || rf->editor_layer.id == 0) {
    return false_v;
  }

  // Use typed message API with typed response
  VkrLayerMsg_EditorGetViewportMapping msg = {
      .h = VKR_LAYER_MSG_HEADER_INIT_NO_PAYLOAD(
          VKR_LAYER_MSG_EDITOR_GET_VIEWPORT_MAPPING),
  };
  msg.h.flags = VKR_LAYER_MSG_FLAG_EXPECTS_RESPONSE;

  VkrLayerRsp_EditorViewportMapping rsp = {0};
  uint64_t rsp_size = 0;
  bool32_t ok = vkr_view_system_send_msg(rf, rf->editor_layer, &msg.h, &rsp,
                                         sizeof(rsp), &rsp_size);
  if (ok && rsp_size == sizeof(rsp) && rsp.h.error == 0) {
    *out_mapping = rsp.mapping;
    return true_v;
  }
  return false_v;
}

bool8_t vkr_view_editor_set_viewport_fit_mode(RendererFrontend *rf,
                                              VkrViewportFitMode mode) {
  if (!rf || !rf->view_system.initialized || rf->editor_layer.id == 0) {
    return false_v;
  }

  VkrLayerMsg_EditorSetViewportFitMode msg = {
      .h = VKR_LAYER_MSG_HEADER_INIT(VKR_LAYER_MSG_EDITOR_SET_VIEWPORT_FIT_MODE,
                                     VkrViewportFitMode),
      .payload = mode,
  };
  return vkr_view_system_send_msg_no_rsp(rf, rf->editor_layer, &msg.h)
             ? true_v
             : false_v;
}

bool8_t vkr_view_editor_set_render_scale(RendererFrontend *rf,
                                         float32_t scale) {
  if (!rf || !rf->view_system.initialized || rf->editor_layer.id == 0) {
    return false_v;
  }

  VkrLayerMsg_EditorSetRenderScale msg = {
      .h = VKR_LAYER_MSG_HEADER_INIT(VKR_LAYER_MSG_EDITOR_SET_RENDER_SCALE,
                                     float32_t),
      .payload = scale,
  };
  return vkr_view_system_send_msg_no_rsp(rf, rf->editor_layer, &msg.h)
             ? true_v
             : false_v;
}

bool8_t
vkr_viewport_mapping_window_to_target_pixel(const VkrViewportMapping *mapping,
                                            int32_t window_x, int32_t window_y,
                                            uint32_t *out_x, uint32_t *out_y) {
  if (!mapping || !out_x || !out_y) {
    return false_v;
  }
  if (mapping->target_width == 0 || mapping->target_height == 0) {
    return false_v;
  }

  // Treat rect values as pixel-aligned; round to be safe.
  const int32_t img_x = (int32_t)vkr_round_f32(mapping->image_rect_px.x);
  const int32_t img_y = (int32_t)vkr_round_f32(mapping->image_rect_px.y);
  const uint32_t img_w =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(mapping->image_rect_px.z));
  const uint32_t img_h =
      vkr_max_u32(1u, (uint32_t)vkr_round_f32(mapping->image_rect_px.w));

  if (window_x < img_x || window_y < img_y) {
    return false_v;
  }

  const int32_t local_x_i = window_x - img_x;
  const int32_t local_y_i = window_y - img_y;
  if (local_x_i < 0 || local_y_i < 0) {
    return false_v;
  }
  if ((uint32_t)local_x_i >= img_w || (uint32_t)local_y_i >= img_h) {
    return false_v;
  }

  const uint32_t local_x = (uint32_t)local_x_i;
  const uint32_t local_y = (uint32_t)local_y_i;

  // Map edges-to-edges for stable picking (top-left -> 0,0; bottom-right ->
  // w-1,h-1).
  uint32_t target_x = 0;
  if (img_w > 1u && mapping->target_width > 1u) {
    target_x = (uint32_t)(((uint64_t)local_x) *
                          (uint64_t)(mapping->target_width - 1u) /
                          (uint64_t)(img_w - 1u));
  }

  uint32_t target_y = 0;
  if (img_h > 1u && mapping->target_height > 1u) {
    target_y = (uint32_t)(((uint64_t)local_y) *
                          (uint64_t)(mapping->target_height - 1u) /
                          (uint64_t)(img_h - 1u));
  }

  *out_x = vkr_min_u32(target_x, mapping->target_width - 1u);
  *out_y = vkr_min_u32(target_y, mapping->target_height - 1u);
  return true_v;
}
