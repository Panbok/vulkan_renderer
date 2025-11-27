#include "renderer/systems/views/vkr_view_ui.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_view_system.h"

typedef struct VkrViewUIState {
  VkrShaderConfig shader_config;
  VkrPipelineHandle pipeline;
  VkrMaterialHandle material;
  VkrRendererInstanceStateHandle instance_state;
  VkrTransform transform;
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

  VkrViewUIState *state =
      arena_alloc(rf->arena, sizeof(VkrViewUIState), ARENA_MEMORY_TAG_STRUCT);
  if (!state) {
    log_error("Failed to allocate UI view state");
    return false_v;
  }
  MemZero(state, sizeof(*state));
  state->transform = vkr_transform_identity();

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
          string8_lit("assets/shaders/default.ui.shadercfg"), rf->scratch_arena,
          &ui_cfg_info, &shadercfg_err)) {
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
  if (vkr_resource_system_load(VKR_RESOURCE_TYPE_MATERIAL,
                               string8_lit("assets/materials/default.ui.mt"),
                               rf->scratch_arena, &default_ui_material_info,
                               &material_load_error)) {
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
}

vkr_internal void vkr_view_ui_on_render(VkrLayerContext *ctx,
                                        const VkrLayerRenderInfo *info) {
  (void)info;
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
      log_error("Failed to bind UI pipeline");
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

  if (state->pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           state->pipeline);
  }
}
