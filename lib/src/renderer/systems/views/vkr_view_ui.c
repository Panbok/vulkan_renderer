#include "renderer/systems/views/vkr_view_ui.h"

#include <stdio.h>

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_clock.h"
#include "math/mat.h"
#include "math/vkr_transform.h"
#include "memory/vkr_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/resources/ui/vkr_ui_text.h"
#include "renderer/systems/vkr_font_system.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_view_system.h"

#define VKR_FPS_UPDATE_INTERVAL 0.25 // Update FPS display every 0.25 seconds
#define VKR_FPS_DELTA_MIN 0.000001   // Minimum delta for FPS calculation
#define VKR_UI_TEXT_PADDING 16.0f

typedef struct VkrViewUIState {
  VkrAllocator temp_allocator;
  Arena *temp_arena;

  // UI rendering
  VkrShaderConfig shader_config;
  VkrPipelineHandle pipeline;
  VkrMaterialHandle material;
  VkrRendererInstanceStateHandle instance_state;
  VkrTransform transform;

  // Text rendering
  VkrShaderConfig text_shader_config;
  VkrPipelineHandle text_pipeline;

  // FPS tracking
  VkrUiText fps_text;
  VkrClock fps_update_clock;
  float64_t current_fps;
  float64_t current_frametime;
  float64_t accumulated_time;
  uint32_t frame_count;
  uint32_t screen_width;
  uint32_t screen_height;
} VkrViewUIState;

vkr_internal bool32_t vkr_view_ui_on_create(VkrLayerContext *ctx);
vkr_internal void vkr_view_ui_on_attach(VkrLayerContext *ctx);
vkr_internal void vkr_view_ui_on_resize(VkrLayerContext *ctx, uint32_t width,
                                        uint32_t height);
vkr_internal void vkr_view_ui_on_render(VkrLayerContext *ctx,
                                        const VkrLayerRenderInfo *info);
vkr_internal void vkr_view_ui_on_detach(VkrLayerContext *ctx);
vkr_internal void vkr_view_ui_on_destroy(VkrLayerContext *ctx);

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
  state->transform = vkr_transform_identity();
  state->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  state->fps_update_clock = vkr_clock_create();
  state->current_fps = 0.0;

  state->temp_arena = arena_create(KB(64), KB(64));
  if (!state->temp_arena) {
    log_error("Failed to create temp arena");
    return false_v;
  }
  state->temp_allocator = (VkrAllocator){.ctx = state->temp_arena};
  if (!vkr_allocator_arena(&state->temp_allocator)) {
    log_error("Failed to create temp allocator");
    return false_v;
  }

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
                    .on_destroy = vkr_view_ui_on_destroy},
      .user_data = state,
  };

  VkrRendererError layer_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_view_system_register_layer(rf, &ui_cfg, &rf->ui_layer, &layer_err)) {
    String8 err = vkr_renderer_get_error_string(layer_err);
    log_error("Failed to register UI view: %s", string8_cstr(&err));
    return false_v;
  }

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
    log_warn("Failed to load default UI material; using built-in default: %s",
             string8_cstr(&error_string));
  }

  state->transform = vkr_transform_from_position_scale_rotation(
      vec3_new(0.0f, 0.0f, 0.0f), vec3_new(150.0f, 150.0f, 1.0f),
      vkr_quat_identity());

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

  if (state->text_shader_config.name.str &&
      state->text_shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, state->text_pipeline,
        state->text_shader_config.name, &alias_err);
  }

  VkrFont *font = vkr_font_system_get_default_mtsdf_font(&rf->font_system);

  VkrUiTextConfig text_config = VKR_UI_TEXT_CONFIG_DEFAULT;
  text_config.font = rf->font_system.default_mtsdf_font_handle;
  text_config.font_size = (float32_t)font->size * 2.0f; // 2x native size
  text_config.color = (Vec4){1.0f, 1.0f, 1.0f, 1.0f};   // White

  VkrRendererError text_create_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_ui_text_create(rf, &rf->allocator, &rf->font_system,
                         state->text_pipeline,
                         string8_lit("FPS: 0.0\nFrametime: 0.0"), &text_config,
                         &state->fps_text, &text_create_err)) {
    vkr_clock_start(&state->fps_update_clock);
  } else {
    String8 err_str = vkr_renderer_get_error_string(text_create_err);
    log_error("Failed to create UI text: %s", string8_cstr(&err_str));
  }

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
  if (state) {
    state->screen_width = width;
    state->screen_height = height;
    VkrTextBounds bounds = vkr_ui_text_get_bounds(&state->fps_text);
    // Position at top-right corner (Y=0 is bottom, Y=height is top)
    float32_t x = (float32_t)width - bounds.size.x - VKR_UI_TEXT_PADDING;
    if (x < 0.0f) {
      x = 0.0f;
    }
    float32_t y = (float32_t)height - bounds.size.y - VKR_UI_TEXT_PADDING;
    vkr_ui_text_set_position(&state->fps_text, vec2_new(x, y));
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

  VkrMaterial *ui_material =
      vkr_material_system_get_by_handle(&rf->material_system, state->material);

  rf->draw_state.instance_state = state->instance_state;

  uint32_t ui_mat_pipeline_id =
      ui_material ? ui_material->pipeline_id : VKR_INVALID_ID;
  const char *ui_shader =
      (ui_material && ui_material->shader_name && ui_material->shader_name[0])
          ? ui_material->shader_name
          : "shader.default.ui";
  if (!vkr_shader_system_use(&rf->shader_system, ui_shader)) {
    vkr_shader_system_use(&rf->shader_system, "shader.default.ui");
  }

  VkrPipelineHandle ui_resolved = VKR_PIPELINE_HANDLE_INVALID;
  VkrRendererError ui_get_err = VKR_RENDERER_ERROR_NONE;
  vkr_pipeline_registry_get_pipeline_for_material(&rf->pipeline_registry,
                                                  ui_shader, ui_mat_pipeline_id,
                                                  &ui_resolved, &ui_get_err);

  if (state->pipeline.id != ui_resolved.id ||
      state->pipeline.generation != ui_resolved.generation) {
    if (state->pipeline.id != 0) {
      vkr_pipeline_registry_release_instance_state(
          &rf->pipeline_registry, state->pipeline, state->instance_state,
          &(VkrRendererError){0});
    }
    VkrRendererError acq_err = VKR_RENDERER_ERROR_NONE;
    if (vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, ui_resolved, &state->instance_state,
            &acq_err)) {
      state->pipeline = ui_resolved;
    } else {
      String8 err_str = vkr_renderer_get_error_string(acq_err);
      log_error("Failed to acquire instance state for resolved UI pipeline: %s",
                string8_cstr(&err_str));
      return;
    }
  }

  VkrPipelineHandle current_pipeline =
      vkr_pipeline_registry_get_current_pipeline(&rf->pipeline_registry);

  VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
  if (current_pipeline.id != ui_resolved.id ||
      current_pipeline.generation != ui_resolved.generation) {
    if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry,
                                             ui_resolved, &bind_err)) {
      String8 err_str = vkr_renderer_get_error_string(bind_err);
      log_error("Failed to bind UI pipeline: %s", string8_cstr(&err_str));
      return;
    }
  }

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_UI);

  vkr_material_system_apply_local(
      &rf->material_system,
      &(VkrLocalMaterialState){.model =
                                   vkr_transform_get_world(&state->transform)});

  if (ui_material) {
    vkr_shader_system_bind_instance(&rf->shader_system,
                                    state->instance_state.id);
    vkr_material_system_apply_instance(&rf->material_system, ui_material,
                                       VKR_PIPELINE_DOMAIN_UI);
  }

  vkr_geometry_system_render(
      rf, &rf->geometry_system,
      vkr_geometry_system_get_default_plane2d(&rf->geometry_system), 1);

  // === FPS/Frametime Calculation ===
  float64_t delta = info->delta_time;

  VkrAllocatorScope scope = vkr_allocator_begin_scope(&state->temp_allocator);
  if (!vkr_allocator_scope_is_valid(&scope)) {
    log_error("Failed to create temp allocator scope");
    return;
  }

  state->accumulated_time += delta;
  state->frame_count++;

  if (vkr_clock_interval_elapsed(&state->fps_update_clock,
                                 VKR_FPS_UPDATE_INTERVAL)) {
    if (state->accumulated_time > 0.0 && state->frame_count > 0) {
      state->current_fps =
          (float64_t)state->frame_count / state->accumulated_time;
      state->current_frametime =
          state->accumulated_time / (float64_t)state->frame_count;
    }
    String8 fps_text = string8_create_formatted(
        &state->temp_allocator, "FPS: %.1f\nFrametime: %.2f ms",
        state->current_fps, state->current_frametime * 1000.0);
    if (fps_text.length > 0) {
      vkr_ui_text_set_content(&state->fps_text, fps_text);
    }

    state->accumulated_time = 0.0;
    state->frame_count = 0;

    VkrTextBounds bounds = vkr_ui_text_get_bounds(&state->fps_text);
    float32_t x =
        (float32_t)state->screen_width - bounds.size.x - VKR_UI_TEXT_PADDING;
    if (x < 0.0f) {
      x = 0.0f;
    }
    // Y=0 is bottom, Y=height is top in this coordinate system
    float32_t y =
        (float32_t)state->screen_height - bounds.size.y - VKR_UI_TEXT_PADDING;
    vkr_ui_text_set_position(&state->fps_text, vec2_new(x, y));
  }

  vkr_ui_text_draw(&state->fps_text);
  vkr_allocator_end_scope(&scope, VKR_ALLOCATOR_MEMORY_TAG_STRING);
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

  vkr_ui_text_destroy(&state->fps_text);

  if (state->text_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           state->text_pipeline);
  }

  if (state->pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           state->pipeline);
  }

  if (state->temp_arena) {
    arena_destroy(state->temp_arena);
  }
}
