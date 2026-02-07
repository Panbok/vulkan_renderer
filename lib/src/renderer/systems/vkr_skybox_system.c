/**
 * @file vkr_skybox_system.c
 * @brief Stateless skybox resources and rendering helper.
 */

#include "renderer/systems/vkr_skybox_system.h"

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_texture_system.h"

bool8_t vkr_skybox_system_init(RendererFrontend *rf, VkrSkyboxSystem *system) {
  if (!rf || !system) {
    return false_v;
  }

  MemZero(system, sizeof(*system));
  system->instance_state.id = VKR_INVALID_ID;

  VkrResourceHandleInfo skybox_cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/default.skybox.shadercfg"),
          &rf->scratch_allocator, &skybox_cfg_info, &shadercfg_err)) {
    String8 err = vkr_renderer_get_error_string(shadercfg_err);
    log_error("Skybox shadercfg load failed: %s", string8_cstr(&err));
    return false_v;
  }

  system->shader_config = *(VkrShaderConfig *)skybox_cfg_info.as.custom;
  if (!vkr_shader_system_create(&rf->shader_system, &system->shader_config)) {
    log_error("Failed to create skybox shader in shader system");
    return false_v;
  }

  VkrRendererError pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &system->shader_config,
          VKR_PIPELINE_DOMAIN_SKYBOX, string8_lit("skybox"), &system->pipeline,
          &pipeline_err)) {
    String8 err_str = vkr_renderer_get_error_string(pipeline_err);
    log_error("Skybox pipeline creation failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  VkrRendererError geom_err = VKR_RENDERER_ERROR_NONE;
  system->cube_geometry = vkr_geometry_system_create_cube(
      &rf->geometry_system, 10.0f, 10.0f, 10.0f, "Skybox Cube", &geom_err);
  if (system->cube_geometry.id == 0) {
    String8 err_str = vkr_renderer_get_error_string(geom_err);
    log_error("Skybox cube geometry creation failed: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  VkrRendererError tex_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_load_cube_map(
          &rf->texture_system, string8_lit("assets/textures/skybox"),
          string8_lit("jpg"), &system->cube_map_texture, &tex_err)) {
    String8 err_str = vkr_renderer_get_error_string(tex_err);
    log_error("Skybox cubemap load failed: %s", string8_cstr(&err_str));
    return false_v;
  }

  VkrRendererError inst_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, system->pipeline, &system->instance_state,
          &inst_err)) {
    String8 err_str = vkr_renderer_get_error_string(inst_err);
    log_error("Skybox instance state acquire failed: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  system->initialized = true_v;
  return true_v;
}

void vkr_skybox_system_shutdown(RendererFrontend *rf,
                                VkrSkyboxSystem *system) {
  if (!rf || !system) {
    return;
  }

  if (system->instance_state.id != VKR_INVALID_ID && system->pipeline.id != 0) {
    VkrRendererError release_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_release_instance_state(
        &rf->pipeline_registry, system->pipeline, system->instance_state,
        &release_err);
    system->instance_state.id = VKR_INVALID_ID;
  }

  if (system->cube_geometry.id != 0) {
    vkr_geometry_system_release(&rf->geometry_system, system->cube_geometry);
    system->cube_geometry = (VkrGeometryHandle){0};
  }

  if (system->cube_map_texture.id != 0) {
    vkr_texture_system_release_by_handle(&rf->texture_system,
                                         system->cube_map_texture);
    system->cube_map_texture = (VkrTextureHandle){0};
  }

  if (system->pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           system->pipeline);
    system->pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  system->initialized = false_v;
}

void vkr_skybox_system_render_packet(RendererFrontend *rf,
                                     const VkrSkyboxSystem *system,
                                     const VkrSkyboxPassPayload *payload,
                                     const VkrFrameGlobals *globals) {
  if (!rf || !system || !payload || !globals || !system->initialized) {
    return;
  }

  if (!vkr_shader_system_use(&rf->shader_system, "shader.default.skybox")) {
    log_error("Failed to use skybox shader");
    return;
  }

  VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
  vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, system->pipeline,
                                      &bind_err);
  if (bind_err != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to bind skybox pipeline");
    return;
  }

  Mat4 sky_view = globals->view;
  sky_view.elements[12] = 0.0f;
  sky_view.elements[13] = 0.0f;
  sky_view.elements[14] = 0.0f;

  VkrGlobalMaterialState skybox_globals = {
      .projection = globals->projection,
      .view = sky_view,
      .ui_projection = mat4_identity(),
      .ui_view = mat4_identity(),
      .ambient_color = globals->ambient_color,
      .view_position = globals->view_position,
      .render_mode = (VkrRenderMode)globals->render_mode,
  };
  vkr_material_system_apply_global(&rf->material_system, &skybox_globals,
                                   VKR_PIPELINE_DOMAIN_SKYBOX);

  vkr_shader_system_bind_instance(&rf->shader_system, system->instance_state.id);

  VkrTextureHandle cubemap =
      payload->cubemap.id != 0 ? payload->cubemap : system->cube_map_texture;
  VkrTexture *cube_map =
      vkr_texture_system_get_by_handle(&rf->texture_system, cubemap);
  if (cube_map && cube_map->handle) {
    if (!vkr_shader_system_sampler_set(&rf->shader_system, "cube_texture",
                                       cube_map->handle)) {
      log_error("Failed to set cube_texture sampler");
    }
  }

  vkr_shader_system_apply_instance(&rf->shader_system);

  vkr_geometry_system_render(rf, &rf->geometry_system, system->cube_geometry, 1);
}
