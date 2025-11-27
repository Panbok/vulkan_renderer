#include "renderer/systems/views/vkr_view_skybox.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/systems/vkr_view_system.h"

typedef struct VkrViewSkyboxState {
  VkrShaderConfig shader_config;
  VkrPipelineHandle pipeline;
  VkrGeometryHandle cube_geometry;
  VkrTextureHandle cube_map_texture;
  VkrRendererInstanceStateHandle instance_state; // Pipeline instance state
  bool8_t initialized;
} VkrViewSkyboxState;

vkr_internal bool32_t vkr_view_skybox_on_create(VkrLayerContext *ctx);
vkr_internal void vkr_view_skybox_on_attach(VkrLayerContext *ctx);
vkr_internal void vkr_view_skybox_on_resize(VkrLayerContext *ctx,
                                            uint32_t width, uint32_t height);
vkr_internal void vkr_view_skybox_on_render(VkrLayerContext *ctx,
                                            const VkrLayerRenderInfo *info);
vkr_internal void vkr_view_skybox_on_detach(VkrLayerContext *ctx);
vkr_internal void vkr_view_skybox_on_destroy(VkrLayerContext *ctx);

bool32_t vkr_view_skybox_register(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (!rf->view_system.initialized) {
    log_error("View system not initialized; cannot register skybox view");
    return false_v;
  }

  // Check if already registered
  if (rf->skybox_layer.id != 0) {
    return true_v;
  }

  VkrLayerPassConfig skybox_passes[1] = {{
      .renderpass_name = string8_lit("Renderpass.Builtin.Skybox"),
      .use_swapchain_color = true_v,
      .use_depth = true_v,
  }};

  VkrViewSkyboxState *state = arena_alloc(rf->arena, sizeof(VkrViewSkyboxState),
                                          ARENA_MEMORY_TAG_STRUCT);
  if (!state) {
    log_error("Failed to allocate skybox view state");
    return false_v;
  }
  MemZero(state, sizeof(*state));

  VkrLayerConfig skybox_cfg = {
      .name = string8_lit("Layer.Skybox"),
      .order = -10, // Render before world (order 0)
      .width = 0,
      .height = 0,
      .view = rf->globals.view,
      .projection = rf->globals.projection,
      .pass_count = ArrayCount(skybox_passes),
      .passes = skybox_passes,
      .callbacks = {.on_create = vkr_view_skybox_on_create,
                    .on_attach = vkr_view_skybox_on_attach,
                    .on_resize = vkr_view_skybox_on_resize,
                    .on_render = vkr_view_skybox_on_render,
                    .on_detach = vkr_view_skybox_on_detach,
                    .on_destroy = vkr_view_skybox_on_destroy},
      .user_data = state,
  };

  VkrRendererError layer_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_view_system_register_layer(rf, &skybox_cfg, &rf->skybox_layer,
                                      &layer_err)) {
    String8 err = vkr_renderer_get_error_string(layer_err);
    log_error("Failed to register skybox view: %s", string8_cstr(&err));
    return false_v;
  }

  log_info("Skybox view registered successfully");
  return true_v;
}

void vkr_view_skybox_unregister(RendererFrontend *rf) {
  assert_log(rf != NULL, "Renderer frontend is NULL");

  if (rf->skybox_layer.id == 0) {
    return;
  }

  vkr_view_system_unregister_layer(rf, rf->skybox_layer);
  rf->skybox_layer = VKR_LAYER_HANDLE_INVALID;
}

vkr_internal bool32_t vkr_view_skybox_on_create(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return false_v;
  }

  VkrViewSkyboxState *state =
      (VkrViewSkyboxState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return false_v;
  }

  // Load skybox shader config
  VkrResourceHandleInfo skybox_cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.skybox.shadercfg"),
          rf->scratch_arena, &skybox_cfg_info, &shadercfg_err)) {
    state->shader_config = *(VkrShaderConfig *)skybox_cfg_info.as.custom;
  } else {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("Skybox shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  // Create shader in the shader system
  if (!vkr_shader_system_create(&rf->shader_system, &state->shader_config)) {
    log_error("Failed to create skybox shader from config");
    return false_v;
  }

  // Create skybox pipeline
  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &state->shader_config,
          VKR_PIPELINE_DOMAIN_SKYBOX, string8_lit("skybox"), &state->pipeline,
          &pipeline_error)) {
    String8 err_str = vkr_renderer_get_error_string(pipeline_error);
    log_error("Skybox pipeline creation failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  VkrRendererError geom_err = VKR_RENDERER_ERROR_NONE;
  state->cube_geometry = vkr_geometry_system_create_cube(
      &rf->geometry_system, 10.0f, 10.0f, 10.0f, "Skybox Cube", &geom_err);
  if (state->cube_geometry.id == 0) {
    String8 err_str = vkr_renderer_get_error_string(geom_err);
    log_error("Skybox cube geometry creation failed: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  // Load cube map texture
  VkrRendererError tex_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_load_cube_map(
          &rf->texture_system, string8_lit("assets/textures/skybox"),
          string8_lit("jpg"), &state->cube_map_texture, &tex_err)) {
    String8 err_str = vkr_renderer_get_error_string(tex_err);
    log_error("Skybox cube map texture load failed: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  // Acquire instance state for the skybox pipeline
  VkrRendererError instance_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, state->pipeline, &state->instance_state,
          &instance_err)) {
    String8 err_str = vkr_renderer_get_error_string(instance_err);
    log_error("Failed to acquire skybox pipeline instance state: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  state->initialized = true_v;
  log_info("Skybox view created successfully");
  return true_v;
}

vkr_internal void vkr_view_skybox_on_attach(VkrLayerContext *ctx) { (void)ctx; }

vkr_internal void vkr_view_skybox_on_resize(VkrLayerContext *ctx,
                                            uint32_t width, uint32_t height) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  vkr_layer_context_set_camera(ctx, &rf->globals.view, &rf->globals.projection);
}

vkr_internal void vkr_view_skybox_on_render(VkrLayerContext *ctx,
                                            const VkrLayerRenderInfo *info) {
  assert_log(ctx != NULL, "Layer context is NULL");
  assert_log(info != NULL, "Layer render info is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    log_error("Renderer frontend is NULL");
    return;
  }

  VkrViewSkyboxState *state =
      (VkrViewSkyboxState *)vkr_layer_context_get_user_data(ctx);
  if (!state || !state->initialized) {
    return;
  }

  // Use the skybox shader
  if (!vkr_shader_system_use(&rf->shader_system, "shader.default.skybox")) {
    log_error("Failed to use skybox shader");
    return;
  }

  // Bind the skybox pipeline
  VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
  vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, state->pipeline,
                                      &bind_err);
  if (bind_err != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to bind skybox pipeline");
    return;
  }

  // Create view matrix without translation (skybox follows camera)
  Mat4 sky_view = rf->globals.view;
  // Zero out translation components (column 3, elements 12-14 in column-major)
  sky_view.elements[12] = 0.0f;
  sky_view.elements[13] = 0.0f;
  sky_view.elements[14] = 0.0f;

  // Apply global uniforms with modified view matrix
  VkrGlobalMaterialState skybox_globals = rf->globals;
  skybox_globals.view = sky_view;
  vkr_material_system_apply_global(&rf->material_system, &skybox_globals,
                                   VKR_PIPELINE_DOMAIN_SKYBOX);

  // Bind our shader instance
  vkr_shader_system_bind_instance(&rf->shader_system, state->instance_state.id);

  // Get the cube map texture and bind it
  VkrTexture *cube_map = vkr_texture_system_get_by_handle(
      &rf->texture_system, state->cube_map_texture);
  if (cube_map && cube_map->handle) {
    // Use shader system to set the cube texture sampler
    if (!vkr_shader_system_sampler_set(&rf->shader_system, "cube_texture",
                                       cube_map->handle)) {
      log_error("Failed to set cube_texture sampler");
    }
  } else {
    log_error("Cube map texture not found or has no handle (handle_id=%u)",
              state->cube_map_texture.id);
  }

  // Apply instance state (includes the texture binding)
  vkr_shader_system_apply_instance(&rf->shader_system);

  // Draw the skybox cube
  vkr_geometry_system_render(rf, &rf->geometry_system, state->cube_geometry, 1);
}

vkr_internal void vkr_view_skybox_on_detach(VkrLayerContext *ctx) { (void)ctx; }

vkr_internal void vkr_view_skybox_on_destroy(VkrLayerContext *ctx) {
  assert_log(ctx != NULL, "Layer context is NULL");

  RendererFrontend *rf =
      (RendererFrontend *)vkr_layer_context_get_renderer(ctx);
  if (!rf) {
    return;
  }

  VkrViewSkyboxState *state =
      (VkrViewSkyboxState *)vkr_layer_context_get_user_data(ctx);
  if (!state) {
    return;
  }

  // Release pipeline instance state
  if (state->instance_state.id != 0 && state->pipeline.id != 0) {
    VkrRendererError release_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_release_instance_state(
        &rf->pipeline_registry, state->pipeline, state->instance_state,
        &release_err);
  }

  // Destroy geometry
  if (state->cube_geometry.id != 0) {
    vkr_geometry_system_release(&rf->geometry_system, state->cube_geometry);
  }

  // Release cube map texture
  if (state->cube_map_texture.id != 0) {
    vkr_texture_system_release_by_handle(&rf->texture_system,
                                         state->cube_map_texture);
  }

  // Destroy pipeline
  if (state->pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           state->pipeline);
  }

  state->initialized = false_v;
  log_info("Skybox view destroyed");
}
