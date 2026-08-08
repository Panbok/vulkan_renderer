/**
 * @file vkr_world_resources.c
 * @brief Shared world pipelines, HDR/IBL state, and 3D text resources.
 */

#include "renderer/systems/vkr_world_resources.h"

#include <stdio.h>

#include "containers/str.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/vec.h"
#include "math/vkr_math.h"
#include "math/vkr_transform.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_render_packet.h"

#define VKR_WORLD_RESOURCES_MAX_TEXTS 16
#define VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE VKR_IBL_IRRADIANCE_SIZE
#define VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE VKR_IBL_PREFILTER_SIZE
#define VKR_WORLD_RESOURCES_IBL_BRDF_SIZE VKR_IBL_BRDF_SIZE
#define VKR_WORLD_RESOURCES_IBL_RENDERPASS_NAME                                \
  "Renderpass.Builtin.IBL.Convolution"

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

vkr_internal VkrTextureOpaqueHandle vkr_world_resources_resolve_backend_texture(
    VkrTextureSystem *texture_system, VkrTextureHandle handle,
    VkrTextureType expected_type) {
  if (!texture_system || handle.id == 0) {
    return NULL;
  }

  VkrTexture *texture =
      vkr_texture_system_get_by_handle(texture_system, handle);
  if (!texture || !texture->handle ||
      texture->description.type != expected_type) {
    return NULL;
  }

  return texture->handle;
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
vkr_world_resources_has_retained_ibl_publisher(const RendererFrontend *rf) {
  return rf && rf->asset_publisher.publish_writable_texture &&
         rf->asset_publisher.bake_ibl_cubemap &&
         rf->asset_publisher.bake_hdr_environment;
}

vkr_internal void vkr_world_resources_release_instance_state(
    RendererFrontend *rf, VkrPipelineHandle pipeline,
    VkrRendererInstanceStateHandle *instance_state) {
  if (!rf || !instance_state || pipeline.id == 0 ||
      instance_state->id == VKR_INVALID_ID) {
    return;
  }

  VkrRendererError release_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_release_instance_state(
          &rf->pipeline_registry, pipeline, *instance_state, &release_err)) {
    String8 err = vkr_renderer_get_error_string(release_err);
    log_warn("World resources: failed to release IBL bake instance state: %s",
             string8_cstr(&err));
  }
  instance_state->id = VKR_INVALID_ID;
}

vkr_internal void
vkr_world_resources_destroy_tonemap_runtime(RendererFrontend *rf,
                                            VkrWorldResources *resources) {
  if (!rf || !resources) {
    return;
  }
  vkr_world_resources_release_instance_state(
      rf, resources->tonemap_pipeline, &resources->tonemap_instance_state);
  if (resources->tonemap_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->tonemap_pipeline);
  }
  resources->tonemap_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->tonemap_instance_state.id = VKR_INVALID_ID;
  resources->tonemap_shader_id = 0u;
  MemZero(&resources->tonemap_shader_config,
          sizeof(resources->tonemap_shader_config));
}

vkr_internal bool8_t vkr_world_resources_init_tonemap_runtime(
    RendererFrontend *rf, VkrWorldResources *resources) {
  VkrResourceHandleInfo config_info = {0};
  VkrRendererError config_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/post.tonemap.shadercfg"),
          &rf->scratch_allocator, &config_info, &config_error)) {
    return false_v;
  }

  resources->tonemap_shader_config = *(VkrShaderConfig *)config_info.as.custom;
  if (!vkr_shader_system_get(&rf->shader_system, "shader.post.tonemap") &&
      !vkr_shader_system_create(&rf->shader_system,
                                &resources->tonemap_shader_config)) {
    vkr_world_resources_destroy_tonemap_runtime(rf, resources);
    return false_v;
  }
  resources->tonemap_shader_id =
      vkr_shader_system_get_id(&rf->shader_system, "shader.post.tonemap");
  if (resources->tonemap_shader_id == 0u) {
    vkr_world_resources_destroy_tonemap_runtime(rf, resources);
    return false_v;
  }

  VkrRendererError pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &resources->tonemap_shader_config,
          VKR_PIPELINE_DOMAIN_POST, string8_lit("post_tonemap"),
          &resources->tonemap_pipeline, &pipeline_error)) {
    vkr_world_resources_destroy_tonemap_runtime(rf, resources);
    return false_v;
  }

  VkrRendererError instance_error = VKR_RENDERER_ERROR_NONE;
  resources->tonemap_instance_state.id = VKR_INVALID_ID;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, resources->tonemap_pipeline,
          &resources->tonemap_instance_state, &instance_error)) {
    vkr_world_resources_destroy_tonemap_runtime(rf, resources);
    return false_v;
  }
  return true_v;
}

vkr_internal uint32_t vkr_world_resources_calculate_mip_count(uint32_t size) {
  uint32_t mips = 1u;
  while (size > 1u) {
    size >>= 1u;
    mips++;
  }
  return mips;
}

vkr_internal uint32_t
vkr_world_resources_ibl_mip_limit(const VkrWorldResources *resources) {
  return resources
             ? Min(resources->hdr_ibl_max_mip_levels, VKR_IBL_MAX_CUBE_MIPS)
             : 0u;
}

vkr_internal bool8_t vkr_world_resources_create_writable_cube_texture(
    RendererFrontend *rf, String8 name, uint32_t size, bool8_t with_mips,
    VkrTextureFormat format, VkrTextureHandle *out_handle) {
  if (!rf || !name.str || !out_handle || size == 0) {
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
  if (!vkr_texture_system_create_writable(&rf->texture_system, name, &desc,
                                          out_handle, &texture_err)) {
    String8 err = vkr_renderer_get_error_string(texture_err);
    log_warn("World resources: failed to create writable cubemap '%.*s': %s",
             (int)name.length, name.str, string8_cstr(&err));
    return false_v;
  }

  return true_v;
}

vkr_internal bool8_t vkr_world_resources_create_writable_2d_texture(
    RendererFrontend *rf, String8 name, uint32_t width, uint32_t height,
    VkrTextureFormat format, VkrTextureHandle *out_handle) {
  if (!rf || !name.str || !out_handle || width == 0u || height == 0u) {
    return false_v;
  }

  VkrTextureDescription desc = {
      .width = width,
      .height = height,
      .channels = 4u,
      .type = VKR_TEXTURE_TYPE_2D,
      .format = format,
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
  };

  VkrRendererError texture_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_create_writable(&rf->texture_system, name, &desc,
                                          out_handle, &texture_error)) {
    String8 error_string = vkr_renderer_get_error_string(texture_error);
    log_warn("World resources: failed to create writable texture '%.*s': %s",
             (int)name.length, name.str, string8_cstr(&error_string));
    return false_v;
  }
  return true_v;
}

vkr_internal VkrRenderPassHandle
vkr_world_resources_ensure_ibl_convolution_renderpass(RendererFrontend *rf,
                                                      bool8_t *out_owned) {
  if (!rf) {
    return NULL;
  }

  VkrRenderPassHandle pass = vkr_renderer_renderpass_get(
      rf, string8_lit(VKR_WORLD_RESOURCES_IBL_RENDERPASS_NAME));
  if (pass) {
    if (out_owned) {
      *out_owned = false_v;
    }
    return pass;
  }

  VkrClearValue clear = {.color_f32 = {0.0f, 0.0f, 0.0f, 1.0f}};
  VkrRenderPassAttachmentDesc color_attachment = {
      .format = VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
      .samples = VKR_SAMPLE_COUNT_1,
      .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
      .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
      .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
      .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
      .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
      .final_layout = VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .clear_value = clear,
  };
  VkrRenderPassDesc pass_desc = {
      .name = string8_lit(VKR_WORLD_RESOURCES_IBL_RENDERPASS_NAME),
      .domain = VKR_PIPELINE_DOMAIN_POST,
      .color_attachment_count = 1,
      .color_attachments = &color_attachment,
      .depth_stencil_attachment = NULL,
      .resolve_attachment_count = 0,
      .resolve_attachments = NULL,
  };

  VkrRendererError pass_err = VKR_RENDERER_ERROR_NONE;
  pass = vkr_renderer_renderpass_create_desc(rf, &pass_desc, &pass_err);
  if (!pass) {
    String8 err = vkr_renderer_get_error_string(pass_err);
    log_warn("World resources: failed to create IBL bake render pass: %s",
             string8_cstr(&err));
    return NULL;
  }

  if (out_owned) {
    *out_owned = true_v;
  }
  return pass;
}

vkr_internal void
vkr_world_resources_destroy_ibl_bake_runtime(RendererFrontend *rf,
                                             VkrWorldResources *resources) {
  if (!rf || !resources) {
    return;
  }

  if (resources->ibl_diffuse_bake_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(
        &rf->pipeline_registry, resources->ibl_diffuse_bake_pipeline);
    resources->ibl_diffuse_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->ibl_specular_bake_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(
        &rf->pipeline_registry, resources->ibl_specular_bake_pipeline);
    resources->ibl_specular_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->ibl_equirect_bake_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(
        &rf->pipeline_registry, resources->ibl_equirect_bake_pipeline);
    resources->ibl_equirect_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->ibl_brdf_bake_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->ibl_brdf_bake_pipeline);
    resources->ibl_brdf_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (resources->ibl_bake_plane_geometry.id != 0) {
    vkr_geometry_system_release(&rf->geometry_system,
                                resources->ibl_bake_plane_geometry);
    resources->ibl_bake_plane_geometry = (VkrGeometryHandle){0};
  }

  if (resources->ibl_bake_render_pass &&
      resources->ibl_bake_render_pass_owned) {
    vkr_renderer_renderpass_destroy(rf, resources->ibl_bake_render_pass);
  }
  resources->ibl_bake_render_pass = NULL;
  resources->ibl_bake_render_pass_owned = false_v;
  resources->ibl_bake_runtime_ready = false_v;
  resources->ibl_equirect_bake_shader_id = 0u;
  resources->ibl_diffuse_bake_shader_id = 0u;
  resources->ibl_specular_bake_shader_id = 0u;
  resources->ibl_brdf_bake_shader_id = 0u;
}

vkr_internal bool8_t vkr_world_resources_ensure_ibl_bake_runtime_ready(
    RendererFrontend *rf, VkrWorldResources *resources) {
  if (!rf || !resources) {
    return false_v;
  }
  if (resources->ibl_bake_runtime_ready) {
    return true_v;
  }

  bool8_t renderpass_owned = false_v;
  VkrRenderPassHandle render_pass =
      vkr_world_resources_ensure_ibl_convolution_renderpass(rf,
                                                            &renderpass_owned);
  if (!render_pass) {
    return false_v;
  }
  resources->ibl_bake_render_pass = render_pass;
  resources->ibl_bake_render_pass_owned = renderpass_owned;

  VkrRendererError geom_err = VKR_RENDERER_ERROR_NONE;
  resources->ibl_bake_plane_geometry = vkr_geometry_system_acquire_by_name(
      &rf->geometry_system, string8_lit("Default Plane"), false_v, &geom_err);
  if (resources->ibl_bake_plane_geometry.id == 0) {
    String8 err = vkr_renderer_get_error_string(geom_err);
    log_warn("World resources: failed to acquire IBL bake plane geometry: %s",
             string8_cstr(&err));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  VkrResourceHandleInfo diffuse_cfg_info = {0};
  VkrRendererError diffuse_cfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/ibl.diffuse_convolution.shadercfg"),
          &rf->scratch_allocator, &diffuse_cfg_info, &diffuse_cfg_err)) {
    String8 err = vkr_renderer_get_error_string(diffuse_cfg_err);
    log_warn("World resources: failed to load diffuse IBL bake shadercfg: %s",
             string8_cstr(&err));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  VkrResourceHandleInfo equirect_cfg_info = {0};
  VkrRendererError equirect_cfg_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/ibl.equirect_to_cube.shadercfg"),
          &rf->scratch_allocator, &equirect_cfg_info, &equirect_cfg_error)) {
    String8 error_string = vkr_renderer_get_error_string(equirect_cfg_error);
    log_warn("World resources: failed to load equirect IBL shadercfg: %s",
             string8_cstr(&error_string));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  VkrResourceHandleInfo brdf_cfg_info = {0};
  VkrRendererError brdf_cfg_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/ibl.brdf_lut.shadercfg"),
          &rf->scratch_allocator, &brdf_cfg_info, &brdf_cfg_error)) {
    String8 error_string = vkr_renderer_get_error_string(brdf_cfg_error);
    log_warn("World resources: failed to load BRDF IBL shadercfg: %s",
             string8_cstr(&error_string));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  VkrResourceHandleInfo specular_cfg_info = {0};
  VkrRendererError specular_cfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/ibl.specular_prefilter.shadercfg"),
          &rf->scratch_allocator, &specular_cfg_info, &specular_cfg_err)) {
    String8 err = vkr_renderer_get_error_string(specular_cfg_err);
    log_warn("World resources: failed to load specular IBL bake shadercfg: %s",
             string8_cstr(&err));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  resources->ibl_diffuse_bake_shader_config =
      *(VkrShaderConfig *)diffuse_cfg_info.as.custom;
  resources->ibl_equirect_bake_shader_config =
      *(VkrShaderConfig *)equirect_cfg_info.as.custom;
  resources->ibl_specular_bake_shader_config =
      *(VkrShaderConfig *)specular_cfg_info.as.custom;
  resources->ibl_brdf_bake_shader_config =
      *(VkrShaderConfig *)brdf_cfg_info.as.custom;

  const VkrShaderConfig *diffuse_cfg =
      &resources->ibl_diffuse_bake_shader_config;
  const VkrShaderConfig *equirect_cfg =
      &resources->ibl_equirect_bake_shader_config;
  const VkrShaderConfig *specular_cfg =
      &resources->ibl_specular_bake_shader_config;
  const VkrShaderConfig *brdf_cfg = &resources->ibl_brdf_bake_shader_config;
  if (!vkr_shader_system_get(&rf->shader_system,
                             "shader.ibl.equirect_to_cube") &&
      !vkr_shader_system_create(&rf->shader_system, equirect_cfg)) {
    log_warn("World resources: failed to register equirect IBL shader");
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }
  if (!vkr_shader_system_get(&rf->shader_system,
                             "shader.ibl.diffuse_convolution") &&
      !vkr_shader_system_create(&rf->shader_system, diffuse_cfg)) {
    log_warn("World resources: failed to register diffuse IBL bake shader");
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }
  if (!vkr_shader_system_get(&rf->shader_system,
                             "shader.ibl.specular_prefilter") &&
      !vkr_shader_system_create(&rf->shader_system, specular_cfg)) {
    log_warn("World resources: failed to register specular IBL bake shader");
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }
  if (!vkr_shader_system_get(&rf->shader_system, "shader.ibl.brdf_lut") &&
      !vkr_shader_system_create(&rf->shader_system, brdf_cfg)) {
    log_warn("World resources: failed to register BRDF IBL shader");
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  resources->ibl_equirect_bake_shader_id = vkr_shader_system_get_id(
      &rf->shader_system, "shader.ibl.equirect_to_cube");
  resources->ibl_diffuse_bake_shader_id = vkr_shader_system_get_id(
      &rf->shader_system, "shader.ibl.diffuse_convolution");
  resources->ibl_specular_bake_shader_id = vkr_shader_system_get_id(
      &rf->shader_system, "shader.ibl.specular_prefilter");
  resources->ibl_brdf_bake_shader_id =
      vkr_shader_system_get_id(&rf->shader_system, "shader.ibl.brdf_lut");
  if (resources->ibl_equirect_bake_shader_id == 0u ||
      resources->ibl_diffuse_bake_shader_id == 0u ||
      resources->ibl_specular_bake_shader_id == 0u ||
      resources->ibl_brdf_bake_shader_id == 0u) {
    log_warn("World resources: failed to cache IBL bake shader IDs");
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  VkrRendererError equirect_pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, equirect_cfg, VKR_PIPELINE_DOMAIN_POST,
          string8_lit("ibl_equirect_bake"),
          &resources->ibl_equirect_bake_pipeline, &equirect_pipeline_error)) {
    String8 error_string =
        vkr_renderer_get_error_string(equirect_pipeline_error);
    log_warn("World resources: failed to create equirect IBL pipeline: %s",
             string8_cstr(&error_string));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  VkrRendererError diffuse_pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, diffuse_cfg, VKR_PIPELINE_DOMAIN_POST,
          string8_lit("ibl_diffuse_bake"),
          &resources->ibl_diffuse_bake_pipeline, &diffuse_pipeline_err)) {
    String8 err = vkr_renderer_get_error_string(diffuse_pipeline_err);
    log_warn("World resources: failed to create diffuse IBL bake pipeline: %s",
             string8_cstr(&err));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  VkrRendererError brdf_pipeline_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, brdf_cfg, VKR_PIPELINE_DOMAIN_POST,
          string8_lit("ibl_brdf_bake"), &resources->ibl_brdf_bake_pipeline,
          &brdf_pipeline_error)) {
    String8 error_string = vkr_renderer_get_error_string(brdf_pipeline_error);
    log_warn("World resources: failed to create BRDF IBL pipeline: %s",
             string8_cstr(&error_string));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  VkrRendererError specular_pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, specular_cfg, VKR_PIPELINE_DOMAIN_POST,
          string8_lit("ibl_specular_bake"),
          &resources->ibl_specular_bake_pipeline, &specular_pipeline_err)) {
    String8 err = vkr_renderer_get_error_string(specular_pipeline_err);
    log_warn("World resources: failed to create specular IBL bake pipeline: %s",
             string8_cstr(&err));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  resources->ibl_bake_runtime_ready = true_v;
  return true_v;
}

vkr_internal void
vkr_world_resources_destroy_target_set(RendererFrontend *rf,
                                       VkrIblPreparedTargetSet *set) {
  if (!rf || !set) {
    return;
  }
  for (uint32_t i = 0; i < set->target_count; ++i) {
    if (set->targets[i]) {
      vkr_renderer_render_target_destroy(rf, set->targets[i]);
    }
  }
  MemZero(set, sizeof(*set));
}

vkr_internal bool8_t vkr_world_resources_prepare_target_set(
    RendererFrontend *rf, VkrWorldResources *resources,
    VkrTextureOpaqueHandle target_texture, uint32_t base_size,
    uint32_t mip_count, VkrIblPreparedTargetSet *out_set) {
  if (!rf || !resources || !target_texture || base_size == 0u ||
      mip_count == 0u || mip_count > VKR_IBL_MAX_CUBE_MIPS || !out_set ||
      !resources->ibl_bake_render_pass) {
    return false_v;
  }

  vkr_world_resources_destroy_target_set(rf, out_set);
  out_set->texture = target_texture;
  out_set->base_size = base_size;
  out_set->mip_count = mip_count;

  for (uint32_t mip = 0; mip < mip_count; ++mip) {
    const uint32_t mip_size = Max(1u, base_size >> mip);
    for (uint32_t face = 0; face < 6u; ++face) {
      const uint32_t target_index = mip * 6u + face;
      VkrRenderTargetAttachmentRef attachment = {
          .texture = target_texture,
          .mip_level = mip,
          .base_layer = face,
          .layer_count = 1u,
      };
      VkrRenderTargetDesc target_desc = {
          .sync_to_window_size = false_v,
          .attachment_count = 1u,
          .attachments = &attachment,
          .width = mip_size,
          .height = mip_size,
      };
      VkrRendererError target_error = VKR_RENDERER_ERROR_NONE;
      out_set->targets[target_index] = vkr_renderer_render_target_create(
          rf, &target_desc, resources->ibl_bake_render_pass, &target_error);
      if (!out_set->targets[target_index]) {
        String8 error_string = vkr_renderer_get_error_string(target_error);
        log_warn("World resources: failed to prepare IBL target face=%u "
                 "mip=%u: %s",
                 face, mip, string8_cstr(&error_string));
        out_set->target_count = target_index;
        vkr_world_resources_destroy_target_set(rf, out_set);
        return false_v;
      }
      out_set->target_count = target_index + 1u;
    }
  }

  out_set->ready = true_v;
  return true_v;
}

vkr_internal bool8_t vkr_world_resources_record_cubemap_face(
    RendererFrontend *rf, VkrWorldResources *resources, uint32_t shader_id,
    VkrPipelineHandle pipeline, VkrRendererInstanceStateHandle instance_state,
    const char *source_binding, VkrTextureOpaqueHandle source_texture,
    VkrRenderTargetHandle target, uint32_t face, uint32_t mip_size,
    bool8_t use_sample_params, float32_t roughness, float32_t source_face_size,
    float32_t source_mip_count) {
  VkrRendererError begin_error = vkr_renderer_begin_render_pass(
      rf, resources->ibl_bake_render_pass, target);
  if (begin_error != VKR_RENDERER_ERROR_NONE) {
    return false_v;
  }

  bool8_t recorded = false_v;
  VkrRendererError bind_error = VKR_RENDERER_ERROR_NONE;
  const Vec4 face_params = {(float32_t)face, 0.0f, 0.0f, 0.0f};
  const Vec4 sample_params = {roughness, source_face_size, source_mip_count,
                              0.0f};
  if (!vkr_shader_system_use_by_id(&rf->shader_system, shader_id) ||
      !vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, pipeline,
                                           &bind_error) ||
      !vkr_shader_system_uniform_set(&rf->shader_system, "face_params",
                                     &face_params) ||
      !vkr_shader_system_apply_global(&rf->shader_system) ||
      !vkr_shader_system_bind_instance(&rf->shader_system, instance_state.id) ||
      !vkr_shader_system_sampler_set(&rf->shader_system, source_binding,
                                     source_texture) ||
      (use_sample_params &&
       !vkr_shader_system_uniform_set(&rf->shader_system, "sample_params",
                                      &sample_params)) ||
      !vkr_shader_system_apply_instance(&rf->shader_system)) {
    goto finish;
  }

  const VkrViewport viewport = {
      .x = 0.0f,
      .y = 0.0f,
      .width = (float32_t)mip_size,
      .height = (float32_t)mip_size,
      .min_depth = 0.0f,
      .max_depth = 1.0f,
  };
  const VkrScissor scissor = {
      .x = 0,
      .y = 0,
      .width = mip_size,
      .height = mip_size,
  };
  vkr_renderer_set_viewport(rf, &viewport);
  vkr_renderer_set_scissor(rf, &scissor);
  vkr_geometry_system_render(rf, &rf->geometry_system,
                             resources->ibl_bake_plane_geometry, 1);
  recorded = true_v;

finish:
  vkr_renderer_end_render_pass(rf);
  return recorded;
}

/** Records a cubemap bake using face/mip attachments prepared beforehand. */
vkr_internal bool8_t vkr_world_resources_bake_cubemap(
    RendererFrontend *rf, VkrWorldResources *resources, String8 shader_name,
    uint32_t shader_id, VkrPipelineHandle pipeline, const char *source_binding,
    VkrTextureOpaqueHandle source_texture,
    const VkrIblPreparedTargetSet *target_set, bool8_t use_sample_params,
    uint32_t source_face_size, uint32_t source_mip_count) {
  if (!rf || !resources || !shader_name.str || shader_id == 0u ||
      pipeline.id == 0 || !source_binding || !source_texture || !target_set ||
      !target_set->ready || !target_set->texture ||
      target_set->base_size == 0u || target_set->mip_count == 0u ||
      !resources->ibl_bake_render_pass) {
    return false_v;
  }

  const float64_t start_seconds = vkr_platform_get_absolute_time();
  VkrRendererInstanceStateHandle instance_state = {.id = VKR_INVALID_ID};
  VkrRendererError instance_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, pipeline, &instance_state, &instance_error)) {
    String8 err = vkr_renderer_get_error_string(instance_error);
    log_error("IBL bake: failed to acquire immutable source state for "
              "'%.*s': %s",
              (int)shader_name.length, shader_name.str, string8_cstr(&err));
    return false_v;
  }

  bool8_t result = false_v;
  for (uint32_t mip = 0; mip < target_set->mip_count; ++mip) {
    const uint32_t mip_size = Max(1u, target_set->base_size >> mip);
    const float32_t roughness =
        target_set->mip_count > 1u
            ? (float32_t)mip / (float32_t)(target_set->mip_count - 1u)
            : 0.0f;
    for (uint32_t face = 0; face < 6u; ++face) {
      const uint32_t target_index = mip * 6u + face;
      if (target_index >= target_set->target_count ||
          !target_set->targets[target_index] ||
          !vkr_world_resources_record_cubemap_face(
              rf, resources, shader_id, pipeline, instance_state,
              source_binding, source_texture, target_set->targets[target_index],
              face, mip_size, use_sample_params, roughness,
              (float32_t)source_face_size, (float32_t)source_mip_count)) {
        goto finish;
      }
    }
  }

  // Make the bake's writes visible to the shader reads that sample this cubemap
  // later in the frame.
  //
  // The bake render pass already moved the image to SHADER_READ_ONLY via its
  // finalLayout, so this is a same-layout barrier -- but a layout transition is
  // not a visibility operation. The subpass→EXTERNAL dependency that render
  // passes are created with is execution-only (dstAccessMask = 0,
  // dstStageMask = BOTTOM_OF_PIPE), so nothing otherwise guarantees a later
  // fragment shader sees these writes. Graph-declared resources get this from
  // the render graph's barriers; these cubemaps are produced inside a pass
  // executor and are invisible to it, so the barrier has to be explicit here.
  const VkrImageSubresourceRange initialized_range = {
      .base_mip = 0u,
      .mip_count = target_set->mip_count,
      .base_layer = 0u,
      .layer_count = 6u,
  };
  VkrRendererError barrier_err = vkr_renderer_image_barrier(
      rf, target_set->texture, VKR_IMAGE_ACCESS_COLOR_ATTACHMENT,
      VKR_IMAGE_ACCESS_SAMPLED, VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &initialized_range);
  if (barrier_err != VKR_RENDERER_ERROR_NONE) {
    String8 err = vkr_renderer_get_error_string(barrier_err);
    log_error(
        "IBL bake: failed to make '%.*s' output visible to shader reads: %s",
        (int)shader_name.length, shader_name.str, string8_cstr(&err));
    goto finish;
  }

  result = true_v;

finish: {
  vkr_world_resources_release_instance_state(rf, pipeline, &instance_state);

  uint64_t output_bytes = 0u;
  for (uint32_t mip = 0u; mip < target_set->mip_count; ++mip) {
    const uint32_t size = Max(1u, target_set->base_size >> mip);
    output_bytes += vkr_texture_format_region_size(
        VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, size, size);
  }
  output_bytes *= 6u;
  const bool8_t conversion =
      vkr_string8_equals_cstr(&shader_name, "shader.ibl.equirect_to_cube");
  const VkrMetricEventProducer producer =
      conversion ? rf->ibl_conversion_metrics : rf->ibl_convolution_metrics;
  (void)vkr_metrics_event_record(
      producer, shader_name, (uint64_t)(start_seconds * 1000000000.0),
      vkr_metrics_elapsed_ns(start_seconds), output_bytes,
      result ? VKR_METRIC_EVENT_STATUS_SUCCESS
             : VKR_METRIC_EVENT_STATUS_FAILED);
  return result;
}
}

vkr_internal bool8_t vkr_world_resources_bake_brdf_lut(
    RendererFrontend *rf, VkrWorldResources *resources) {
  if (!rf || !resources || resources->ibl_brdf_baked ||
      resources->ibl_brdf_bake_pipeline.id == 0 ||
      resources->ibl_brdf_bake_shader_id == 0u ||
      !resources->ibl_brdf_bake_target) {
    return resources && resources->ibl_brdf_baked;
  }

  const float64_t start_seconds = vkr_platform_get_absolute_time();
  bool8_t result = false_v;
  VkrRendererError begin_error = vkr_renderer_begin_render_pass(
      rf, resources->ibl_bake_render_pass, resources->ibl_brdf_bake_target);
  if (begin_error != VKR_RENDERER_ERROR_NONE) {
    goto finish;
  }

  if (!vkr_shader_system_use_by_id(&rf->shader_system,
                                   resources->ibl_brdf_bake_shader_id)) {
    vkr_renderer_end_render_pass(rf);
    goto finish;
  }

  VkrRendererError bind_error = VKR_RENDERER_ERROR_NONE;
  bool8_t baked = vkr_pipeline_registry_bind_pipeline(
      &rf->pipeline_registry, resources->ibl_brdf_bake_pipeline, &bind_error);
  if (baked) {
    VkrViewport viewport = {.x = 0.0f,
                            .y = 0.0f,
                            .width = VKR_WORLD_RESOURCES_IBL_BRDF_SIZE,
                            .height = VKR_WORLD_RESOURCES_IBL_BRDF_SIZE,
                            .min_depth = 0.0f,
                            .max_depth = 1.0f};
    VkrScissor scissor = {.x = 0,
                          .y = 0,
                          .width = VKR_WORLD_RESOURCES_IBL_BRDF_SIZE,
                          .height = VKR_WORLD_RESOURCES_IBL_BRDF_SIZE};
    vkr_renderer_set_viewport(rf, &viewport);
    vkr_renderer_set_scissor(rf, &scissor);
    vkr_renderer_draw(rf, 3u, 1u, 0u, 0u);
  }
  vkr_renderer_end_render_pass(rf);
  if (!baked) {
    goto finish;
  }

  VkrTextureOpaqueHandle target = vkr_world_resources_resolve_backend_texture(
      &rf->texture_system, resources->ibl_brdf_lut, VKR_TEXTURE_TYPE_2D);
  if (!target ||
      vkr_renderer_image_barrier(rf, target, VKR_IMAGE_ACCESS_COLOR_ATTACHMENT,
                                 VKR_IMAGE_ACCESS_SAMPLED,
                                 VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                 NULL) != VKR_RENDERER_ERROR_NONE) {
    goto finish;
  }
  resources->ibl_brdf_baked = true_v;
  result = true_v;

finish:
  (void)vkr_metrics_event_record(
      rf->ibl_convolution_metrics, string8_lit("shader.ibl.brdf_lut"),
      (uint64_t)(start_seconds * 1000000000.0),
      vkr_metrics_elapsed_ns(start_seconds),
      vkr_texture_format_region_size(VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
                                     VKR_WORLD_RESOURCES_IBL_BRDF_SIZE,
                                     VKR_WORLD_RESOURCES_IBL_BRDF_SIZE),
      result ? VKR_METRIC_EVENT_STATUS_SUCCESS
             : VKR_METRIC_EVENT_STATUS_FAILED);
  return result;
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
          VKR_PIPELINE_DOMAIN_WORLD, string8_lit("world"), &resources->pipeline,
          &pipeline_err)) {
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

  {
    VkrResourceHandleInfo pbr_cfg_info = {0};
    VkrRendererError pbr_cfg_err = VKR_RENDERER_ERROR_NONE;
    if (vkr_resource_system_load_custom(
            string8_lit("shadercfg"),
            string8_lit("assets/shaders/pbr.world.shadercfg"),
            &rf->scratch_allocator, &pbr_cfg_info, &pbr_cfg_err)) {
      resources->pbr_shader_config = *(VkrShaderConfig *)pbr_cfg_info.as.custom;

      resources->pbr_world_shader_config = resources->pbr_shader_config;
      resources->pbr_transparent_shader_config = resources->pbr_shader_config;
      resources->pbr_overlay_shader_config = resources->pbr_shader_config;
      resources->pbr_double_sided_shader_config = resources->pbr_shader_config;
      resources->pbr_transparent_double_sided_shader_config =
          resources->pbr_shader_config;
      resources->pbr_overlay_double_sided_shader_config =
          resources->pbr_shader_config;

      resources->pbr_world_shader_config.name = string8_lit("shader.pbr.world");
      resources->pbr_transparent_shader_config.name =
          string8_lit("shader.pbr.world.transparent");
      resources->pbr_overlay_shader_config.name =
          string8_lit("shader.pbr.world.overlay");
      resources->pbr_double_sided_shader_config.name =
          string8_lit("shader.pbr.world.double_sided");
      resources->pbr_transparent_double_sided_shader_config.name =
          string8_lit("shader.pbr.world.transparent.double_sided");
      resources->pbr_overlay_double_sided_shader_config.name =
          string8_lit("shader.pbr.world.overlay.double_sided");
      resources->pbr_double_sided_shader_config.cull_mode = VKR_CULL_MODE_NONE;
      resources->pbr_transparent_double_sided_shader_config.cull_mode =
          VKR_CULL_MODE_NONE;
      resources->pbr_overlay_double_sided_shader_config.cull_mode =
          VKR_CULL_MODE_NONE;

      if (!vkr_shader_system_create(&rf->shader_system,
                                    &resources->pbr_world_shader_config) ||
          !vkr_shader_system_create(
              &rf->shader_system, &resources->pbr_transparent_shader_config) ||
          !vkr_shader_system_create(&rf->shader_system,
                                    &resources->pbr_overlay_shader_config) ||
          !vkr_shader_system_create(
              &rf->shader_system, &resources->pbr_double_sided_shader_config) ||
          !vkr_shader_system_create(
              &rf->shader_system,
              &resources->pbr_transparent_double_sided_shader_config) ||
          !vkr_shader_system_create(
              &rf->shader_system,
              &resources->pbr_overlay_double_sided_shader_config)) {
        log_warn("Failed to register PBR world shaders");
      } else {
        VkrRendererError pbr_world_err = VKR_RENDERER_ERROR_NONE;
        if (!vkr_pipeline_registry_create_from_shader_config(
                &rf->pipeline_registry, &resources->pbr_world_shader_config,
                VKR_PIPELINE_DOMAIN_WORLD, string8_lit("pbr_world"),
                &resources->pbr_pipeline, &pbr_world_err)) {
          String8 err_str = vkr_renderer_get_error_string(pbr_world_err);
          log_warn("PBR world pipeline creation failed: %s",
                   string8_cstr(&err_str));
          resources->pbr_pipeline = VKR_PIPELINE_HANDLE_INVALID;
        }

        VkrRendererError pbr_transparent_err = VKR_RENDERER_ERROR_NONE;
        if (!vkr_pipeline_registry_create_from_shader_config(
                &rf->pipeline_registry,
                &resources->pbr_transparent_shader_config,
                VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT,
                string8_lit("pbr_world_transparent"),
                &resources->pbr_transparent_pipeline, &pbr_transparent_err)) {
          String8 err_str = vkr_renderer_get_error_string(pbr_transparent_err);
          log_warn("PBR transparent pipeline creation failed: %s",
                   string8_cstr(&err_str));
          resources->pbr_transparent_pipeline = VKR_PIPELINE_HANDLE_INVALID;
        }

        VkrRendererError pbr_overlay_err = VKR_RENDERER_ERROR_NONE;
        if (!vkr_pipeline_registry_create_from_shader_config(
                &rf->pipeline_registry, &resources->pbr_overlay_shader_config,
                VKR_PIPELINE_DOMAIN_WORLD_OVERLAY,
                string8_lit("pbr_world_overlay"),
                &resources->pbr_overlay_pipeline, &pbr_overlay_err)) {
          String8 err_str = vkr_renderer_get_error_string(pbr_overlay_err);
          log_warn("PBR overlay pipeline creation failed: %s",
                   string8_cstr(&err_str));
          resources->pbr_overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
        }

        VkrRendererError pbr_double_sided_err = VKR_RENDERER_ERROR_NONE;
        if (!vkr_pipeline_registry_create_from_shader_config(
                &rf->pipeline_registry,
                &resources->pbr_double_sided_shader_config,
                VKR_PIPELINE_DOMAIN_WORLD,
                string8_lit("pbr_world_double_sided"),
                &resources->pbr_double_sided_pipeline, &pbr_double_sided_err)) {
          String8 err_str = vkr_renderer_get_error_string(pbr_double_sided_err);
          log_warn("PBR double-sided pipeline creation failed: %s",
                   string8_cstr(&err_str));
          resources->pbr_double_sided_pipeline = VKR_PIPELINE_HANDLE_INVALID;
        }

        VkrRendererError pbr_transparent_double_sided_err =
            VKR_RENDERER_ERROR_NONE;
        if (!vkr_pipeline_registry_create_from_shader_config(
                &rf->pipeline_registry,
                &resources->pbr_transparent_double_sided_shader_config,
                VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT,
                string8_lit("pbr_world_transparent_double_sided"),
                &resources->pbr_transparent_double_sided_pipeline,
                &pbr_transparent_double_sided_err)) {
          String8 err_str =
              vkr_renderer_get_error_string(pbr_transparent_double_sided_err);
          log_warn("PBR transparent double-sided pipeline creation failed: %s",
                   string8_cstr(&err_str));
          resources->pbr_transparent_double_sided_pipeline =
              VKR_PIPELINE_HANDLE_INVALID;
        }

        VkrRendererError pbr_overlay_double_sided_err = VKR_RENDERER_ERROR_NONE;
        if (!vkr_pipeline_registry_create_from_shader_config(
                &rf->pipeline_registry,
                &resources->pbr_overlay_double_sided_shader_config,
                VKR_PIPELINE_DOMAIN_WORLD_OVERLAY,
                string8_lit("pbr_world_overlay_double_sided"),
                &resources->pbr_overlay_double_sided_pipeline,
                &pbr_overlay_double_sided_err)) {
          String8 err_str =
              vkr_renderer_get_error_string(pbr_overlay_double_sided_err);
          log_warn("PBR overlay double-sided pipeline creation failed: %s",
                   string8_cstr(&err_str));
          resources->pbr_overlay_double_sided_pipeline =
              VKR_PIPELINE_HANDLE_INVALID;
        }
      }
    } else {
      String8 err = vkr_renderer_get_error_string(pbr_cfg_err);
      log_warn("PBR shadercfg load failed: %s", string8_cstr(&err));
    }
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
    log_warn("World text pipeline creation failed: %s", string8_cstr(&err_str));
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

  resources->tonemap_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->tonemap_instance_state.id = VKR_INVALID_ID;
  resources->tonemap_shader_id = 0u;
  if (!vkr_world_resources_init_tonemap_runtime(rf, resources)) {
    log_error("World resources: HDR tonemap runtime initialization failed");
    goto cleanup;
  }

  resources->ibl_fallback_source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_brdf_lut = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_bake_render_pass = NULL;
  resources->ibl_equirect_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->ibl_diffuse_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->ibl_specular_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->ibl_brdf_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->ibl_equirect_bake_shader_id = 0u;
  resources->ibl_diffuse_bake_shader_id = 0u;
  resources->ibl_specular_bake_shader_id = 0u;
  resources->ibl_brdf_bake_shader_id = 0u;
  resources->ibl_bake_plane_geometry = (VkrGeometryHandle){0};
  resources->ibl_active_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_enabled = false_v;
  resources->ibl_active_intensity = 1.0f;
  resources->ibl_active_diffuse_intensity = 1.0f;
  resources->ibl_active_specular_intensity = 1.0f;
  resources->ibl_bake_runtime_ready = false_v;
  resources->ibl_bake_render_pass_owned = false_v;
  resources->ibl_default_ready = false_v;
  resources->ibl_default_prepared = false_v;
  resources->ibl_brdf_baked = false_v;

  VkrDeviceInformation device_information = {0};
  vkr_renderer_get_device_information(rf, &device_information,
                                      rf->scratch_arena);
  resources->supports_hdr_ibl = device_information.supports_hdr_ibl;
  resources->hdr_ibl_max_cube_extent =
      device_information.hdr_ibl_max_cube_extent;
  resources->hdr_ibl_max_mip_levels = device_information.hdr_ibl_max_mip_levels;

  if (resources->supports_hdr_ibl &&
      !vkr_world_resources_ensure_ibl_bake_runtime_ready(rf, resources)) {
    log_warn("World resources: HDR IBL runtime unavailable; fallback maps "
             "will remain active");
    resources->supports_hdr_ibl = false_v;
  }

  resources->initialized = true_v;
  return true_v;

cleanup:
  vkr_world_resources_destroy_tonemap_runtime(rf, resources);
  vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
  if (resources->text_slots.data) {
    array_destroy_VkrWorldTextSlot(&resources->text_slots);
    resources->text_slots = (Array_VkrWorldTextSlot){0};
  }
  if (resources->text_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->text_pipeline);
    resources->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_overlay_double_sided_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(
        &rf->pipeline_registry, resources->pbr_overlay_double_sided_pipeline);
    resources->pbr_overlay_double_sided_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_transparent_double_sided_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(
        &rf->pipeline_registry,
        resources->pbr_transparent_double_sided_pipeline);
    resources->pbr_transparent_double_sided_pipeline =
        VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_double_sided_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(
        &rf->pipeline_registry, resources->pbr_double_sided_pipeline);
    resources->pbr_double_sided_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_overlay_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->pbr_overlay_pipeline);
    resources->pbr_overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_transparent_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->pbr_transparent_pipeline);
    resources->pbr_transparent_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->pbr_pipeline);
    resources->pbr_pipeline = VKR_PIPELINE_HANDLE_INVALID;
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
  MemZero(&resources->pbr_shader_config, sizeof(resources->pbr_shader_config));
  MemZero(&resources->pbr_world_shader_config,
          sizeof(resources->pbr_world_shader_config));
  MemZero(&resources->pbr_transparent_shader_config,
          sizeof(resources->pbr_transparent_shader_config));
  MemZero(&resources->pbr_overlay_shader_config,
          sizeof(resources->pbr_overlay_shader_config));
  MemZero(&resources->pbr_double_sided_shader_config,
          sizeof(resources->pbr_double_sided_shader_config));
  MemZero(&resources->pbr_transparent_double_sided_shader_config,
          sizeof(resources->pbr_transparent_double_sided_shader_config));
  MemZero(&resources->pbr_overlay_double_sided_shader_config,
          sizeof(resources->pbr_overlay_double_sided_shader_config));
  MemZero(&resources->text_shader_config,
          sizeof(resources->text_shader_config));
  return false_v;
}

bool8_t vkr_world_resources_init_retained(RendererFrontend *rf,
                                          VkrWorldResources *resources) {
  if (!rf || !resources) {
    return false_v;
  }
  MemZero(resources, sizeof(*resources));
  resources->pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->transparent_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->tonemap_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->tonemap_instance_state.id = VKR_INVALID_ID;
  resources->ibl_fallback_source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_brdf_lut = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_intensity = 1.0f;
  resources->ibl_active_diffuse_intensity = 1.0f;
  resources->ibl_active_specular_intensity = 1.0f;
  resources->supports_hdr_ibl = true_v;
  resources->hdr_ibl_max_cube_extent = VKR_IBL_PREFILTER_SIZE;
  resources->hdr_ibl_max_mip_levels = VKR_IBL_PREFILTER_MIP_COUNT;
  resources->text_slots = array_create_VkrWorldTextSlot(
      &rf->allocator, VKR_WORLD_RESOURCES_MAX_TEXTS);
  if (!resources->text_slots.data) {
    return false_v;
  }
  MemZero(resources->text_slots.data,
          sizeof(VkrWorldTextSlot) * (uint64_t)resources->text_slots.length);
  resources->initialized = true_v;
  return true_v;
}

bool8_t vkr_world_resources_prepare_default_ibl(RendererFrontend *rf,
                                                VkrWorldResources *resources) {
  if (!rf || !resources) {
    return false_v;
  }
  if (resources->ibl_default_prepared) {
    return true_v;
  }

  VkrTextureHandle fallback_source = VKR_TEXTURE_HANDLE_INVALID;
  if (rf->skybox_system.initialized &&
      rf->skybox_system.cube_map_texture.id != 0) {
    fallback_source = rf->skybox_system.cube_map_texture;
    vkr_texture_system_add_ref_by_handle(&rf->texture_system, fallback_source);
  } else {
    VkrRendererError cube_error = VKR_RENDERER_ERROR_NONE;
    if (!vkr_texture_system_load_cube_map(
            &rf->texture_system, string8_lit("assets/textures/skybox"),
            string8_lit("jpg"), &fallback_source, &cube_error)) {
      String8 err_str = vkr_renderer_get_error_string(cube_error);
      log_error(
          "World resources: failed to initialize fallback IBL cubemap: %s",
          string8_cstr(&err_str));
      return false_v;
    }
  }

  resources->ibl_legacy_fallback_source_cubemap = fallback_source;
  resources->ibl_fallback_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;

  if (!resources->supports_hdr_ibl) {
    resources->ibl_fallback_source_cubemap = fallback_source;
    resources->ibl_brdf_lut =
        vkr_texture_system_get_default_specular_handle(&rf->texture_system);
    resources->ibl_default_prepared = true_v;
    return true_v;
  }

  VkrTextureHandle irradiance = VKR_TEXTURE_HANDLE_INVALID;
  VkrTextureHandle prefilter = VKR_TEXTURE_HANDLE_INVALID;
  VkrTextureHandle brdf = VKR_TEXTURE_HANDLE_INVALID;
  VkrTextureHandle delivery = VKR_TEXTURE_HANDLE_INVALID;
  VkrTextureHandle source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  VkrTexturePreparedLoad prepared_hdr = {0};
  VkrRendererError hdr_error = VKR_RENDERER_ERROR_NONE;
  const String8 hdr_path =
      string8_lit("assets/textures/citrus_orchard_puresky_4k.hdr");
  if (!vkr_texture_system_prepare_load_from_file(
          &rf->texture_system, hdr_path, VKR_TEXTURE_RGBA_CHANNELS,
          &rf->scratch_allocator, &prepared_hdr, &hdr_error) ||
      prepared_hdr.description.format !=
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT ||
      !vkr_texture_system_finalize_prepared_load(&rf->texture_system, hdr_path,
                                                 &prepared_hdr, &delivery,
                                                 &hdr_error)) {
    vkr_texture_system_release_prepared_load(&prepared_hdr);
    log_warn("World resources: failed to prepare base HDR environment");
    goto cleanup;
  }
  vkr_texture_system_release_prepared_load(&prepared_hdr);
  vkr_texture_system_add_ref_by_handle(&rf->texture_system, delivery);

  VkrTexture *delivery_texture =
      vkr_texture_system_get_by_handle(&rf->texture_system, delivery);
  uint32_t source_face_size = 0u;
  uint32_t source_mip_count = 0u;
  if (!delivery_texture ||
      !vkr_ibl_derive_cubemap_size(delivery_texture->description.width,
                                   delivery_texture->description.height,
                                   resources->hdr_ibl_max_cube_extent,
                                   vkr_world_resources_ibl_mip_limit(resources),
                                   &source_face_size, &source_mip_count) ||
      !vkr_world_resources_create_writable_cube_texture(
          rf, string8_lit("__ibl.default.source"), source_face_size, true_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, &source_cubemap) ||
      !vkr_world_resources_create_writable_cube_texture(
          rf, string8_lit("__ibl.default.irradiance"),
          VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, false_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, &irradiance) ||
      !vkr_world_resources_create_writable_cube_texture(
          rf, string8_lit("__ibl.default.prefilter"),
          VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE, true_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, &prefilter)) {
    goto cleanup;
  }

  if (vkr_world_resources_has_retained_ibl_publisher(rf)) {
    if (!rf->asset_publisher.bake_hdr_environment(rf->asset_publisher.state,
                                                  delivery, source_cubemap,
                                                  irradiance, prefilter)) {
      goto cleanup;
    }
    resources->ibl_fallback_source_cubemap = source_cubemap;
    resources->ibl_fallback_irradiance_cubemap = irradiance;
    resources->ibl_fallback_prefilter_cubemap = prefilter;
    resources->ibl_brdf_lut =
        vkr_texture_system_get_default_specular_handle(&rf->texture_system);
    resources->ibl_default_delivery_equirect = delivery;
    (void)vkr_world_resources_release_texture(
        &rf->texture_system, &resources->ibl_default_delivery_equirect);
    (void)vkr_world_resources_release_texture(
        &rf->texture_system, &resources->ibl_legacy_fallback_source_cubemap);
    resources->ibl_default_prepared = true_v;
    resources->ibl_default_ready = true_v;
    return true_v;
  }

  if (!vkr_world_resources_create_writable_2d_texture(
          rf, string8_lit("__ibl.default.brdf_lut"),
          VKR_WORLD_RESOURCES_IBL_BRDF_SIZE, VKR_WORLD_RESOURCES_IBL_BRDF_SIZE,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT, &brdf)) {
    goto cleanup;
  }

  VkrTextureOpaqueHandle source_texture =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, source_cubemap, VKR_TEXTURE_TYPE_CUBE_MAP);
  VkrTextureOpaqueHandle irradiance_texture =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, irradiance, VKR_TEXTURE_TYPE_CUBE_MAP);
  VkrTextureOpaqueHandle prefilter_texture =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, prefilter, VKR_TEXTURE_TYPE_CUBE_MAP);
  VkrTextureOpaqueHandle brdf_texture =
      vkr_world_resources_resolve_backend_texture(&rf->texture_system, brdf,
                                                  VKR_TEXTURE_TYPE_2D);
  if (!source_texture || !irradiance_texture || !prefilter_texture ||
      !brdf_texture ||
      !vkr_world_resources_prepare_target_set(
          rf, resources, source_texture, source_face_size, source_mip_count,
          &resources->ibl_default_source_targets) ||
      !vkr_world_resources_prepare_target_set(
          rf, resources, irradiance_texture,
          VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, 1u,
          &resources->ibl_default_irradiance_targets) ||
      !vkr_world_resources_prepare_target_set(
          rf, resources, prefilter_texture,
          VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE,
          vkr_world_resources_calculate_mip_count(
              VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE),
          &resources->ibl_default_prefilter_targets)) {
    goto cleanup;
  }

  VkrRenderTargetAttachmentRef brdf_attachment = {
      .texture = brdf_texture,
      .mip_level = 0u,
      .base_layer = 0u,
      .layer_count = 1u,
  };
  VkrRenderTargetDesc brdf_target_desc = {
      .sync_to_window_size = false_v,
      .attachment_count = 1u,
      .attachments = &brdf_attachment,
      .width = VKR_WORLD_RESOURCES_IBL_BRDF_SIZE,
      .height = VKR_WORLD_RESOURCES_IBL_BRDF_SIZE,
  };
  VkrRendererError target_error = VKR_RENDERER_ERROR_NONE;
  resources->ibl_brdf_bake_target = vkr_renderer_render_target_create(
      rf, &brdf_target_desc, resources->ibl_bake_render_pass, &target_error);
  if (!resources->ibl_brdf_bake_target) {
    goto cleanup;
  }

  resources->ibl_fallback_irradiance_cubemap = irradiance;
  resources->ibl_fallback_prefilter_cubemap = prefilter;
  resources->ibl_fallback_source_cubemap = source_cubemap;
  resources->ibl_default_delivery_equirect = delivery;
  resources->ibl_brdf_lut = brdf;
  resources->ibl_default_prepared = true_v;
  return true_v;

cleanup:
  vkr_world_resources_destroy_target_set(
      rf, &resources->ibl_default_source_targets);
  vkr_world_resources_destroy_target_set(
      rf, &resources->ibl_default_irradiance_targets);
  vkr_world_resources_destroy_target_set(
      rf, &resources->ibl_default_prefilter_targets);
  if (resources->ibl_brdf_bake_target) {
    vkr_renderer_render_target_destroy(rf, resources->ibl_brdf_bake_target);
    resources->ibl_brdf_bake_target = NULL;
  }
  vkr_world_resources_release_texture(&rf->texture_system, &brdf);
  vkr_world_resources_release_texture(&rf->texture_system, &prefilter);
  vkr_world_resources_release_texture(&rf->texture_system, &irradiance);
  vkr_world_resources_release_texture(&rf->texture_system, &source_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system, &delivery);
  vkr_world_resources_release_texture(&rf->texture_system, &fallback_source);
  resources->ibl_fallback_source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_legacy_fallback_source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  return false_v;
}

bool8_t
vkr_world_resources_ensure_default_ibl_ready(RendererFrontend *rf,
                                             VkrWorldResources *resources) {
  if (!rf || !resources || !resources->ibl_default_prepared) {
    return false_v;
  }
  if (resources->ibl_default_ready) {
    return true_v;
  }

  if (!resources->supports_hdr_ibl) {
    resources->ibl_fallback_irradiance_cubemap =
        resources->ibl_fallback_source_cubemap;
    resources->ibl_fallback_prefilter_cubemap =
        resources->ibl_fallback_source_cubemap;
    vkr_texture_system_add_ref_by_handle(
        &rf->texture_system, resources->ibl_fallback_source_cubemap);
    vkr_texture_system_add_ref_by_handle(
        &rf->texture_system, resources->ibl_fallback_source_cubemap);
    if (!resources->hdr_capability_failure_logged) {
      log_warn("HDR IBL is unsupported on this device; using the legacy LDR "
               "fallback without creating 8-bit resources under HDR handles");
      resources->hdr_capability_failure_logged = true_v;
    }
    resources->ibl_default_ready = true_v;
    return true_v;
  }

  VkrTexture *source = vkr_texture_system_get_by_handle(
      &rf->texture_system, resources->ibl_fallback_source_cubemap);
  if (!source || !source->handle) {
    return false_v;
  }

  if (!resources->ibl_default_cube_baked) {
    VkrTextureOpaqueHandle delivery =
        vkr_world_resources_resolve_backend_texture(
            &rf->texture_system, resources->ibl_default_delivery_equirect,
            VKR_TEXTURE_TYPE_2D);
    if (!delivery ||
        vkr_renderer_image_barrier(rf, delivery, VKR_IMAGE_ACCESS_TRANSFER_DST,
                                   VKR_IMAGE_ACCESS_SAMPLED,
                                   VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   NULL) != VKR_RENDERER_ERROR_NONE ||
        !vkr_world_resources_bake_cubemap(
            rf, resources, string8_lit("shader.ibl.equirect_to_cube"),
            resources->ibl_equirect_bake_shader_id,
            resources->ibl_equirect_bake_pipeline, "source_equirect", delivery,
            &resources->ibl_default_source_targets, false_v, 1u, 1u)) {
      return false_v;
    }
    vkr_world_resources_release_texture(
        &rf->texture_system, &resources->ibl_default_delivery_equirect);
    resources->ibl_default_cube_baked = true_v;
  }

  if (!vkr_world_resources_bake_brdf_lut(rf, resources) ||
      !vkr_world_resources_bake_cubemap(
          rf, resources, string8_lit("shader.ibl.diffuse_convolution"),
          resources->ibl_diffuse_bake_shader_id,
          resources->ibl_diffuse_bake_pipeline, "source_cubemap",
          source->handle, &resources->ibl_default_irradiance_targets, false_v,
          source->description.width,
          resources->ibl_default_source_targets.mip_count) ||
      !vkr_world_resources_bake_cubemap(
          rf, resources, string8_lit("shader.ibl.specular_prefilter"),
          resources->ibl_specular_bake_shader_id,
          resources->ibl_specular_bake_pipeline, "source_cubemap",
          source->handle, &resources->ibl_default_prefilter_targets, true_v,
          source->description.width,
          resources->ibl_default_source_targets.mip_count)) {
    return false_v;
  }

  resources->ibl_default_ready = true_v;
  vkr_world_resources_destroy_target_set(
      rf, &resources->ibl_default_source_targets);
  vkr_world_resources_destroy_target_set(
      rf, &resources->ibl_default_irradiance_targets);
  vkr_world_resources_destroy_target_set(
      rf, &resources->ibl_default_prefilter_targets);
  if (resources->ibl_brdf_bake_target) {
    vkr_renderer_render_target_destroy(rf, resources->ibl_brdf_bake_target);
    resources->ibl_brdf_bake_target = NULL;
  }
  return true_v;
}

void vkr_world_resources_release_scene_environment_targets(RendererFrontend *rf,
                                                           VkrScene *scene) {
  if (!rf || !scene) {
    return;
  }
  vkr_world_resources_destroy_target_set(rf, &scene->environment.cube_targets);
  vkr_world_resources_destroy_target_set(
      rf, &scene->environment.irradiance_targets);
  vkr_world_resources_destroy_target_set(rf,
                                         &scene->environment.prefilter_targets);
}

vkr_internal void
vkr_world_resources_fail_scene_environment(RendererFrontend *rf,
                                           VkrScene *scene) {
  if (!rf || !scene) {
    return;
  }

  VkrSceneEnvironment *environment = &scene->environment;
  vkr_world_resources_release_scene_environment_targets(rf, scene);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &environment->prefilter_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &environment->irradiance_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &environment->source_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &environment->delivery_equirect);
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
}

vkr_internal bool8_t vkr_world_resources_prepare_published_environment(
    RendererFrontend *rf, VkrWorldResources *resources, VkrScene *scene) {
  VkrSceneEnvironment *environment = &scene->environment;
  if (environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT) {
    VkrTexture *delivery = vkr_texture_system_get_by_handle(
        &rf->texture_system, environment->delivery_equirect);
    if (!delivery || !delivery->handle ||
        delivery->description.type != VKR_TEXTURE_TYPE_2D ||
        delivery->description.format !=
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT ||
        !rf->asset_publisher.bake_hdr_environment ||
        !vkr_ibl_derive_cubemap_size(
            delivery->description.width, delivery->description.height,
            resources->hdr_ibl_max_cube_extent,
            vkr_world_resources_ibl_mip_limit(resources),
            &environment->source_face_size, &environment->source_mip_count)) {
      goto failed;
    }
  } else if (environment->source_kind == VKR_SCENE_ENV_SOURCE_CUBEMAP) {
    VkrTexture *source = vkr_texture_system_get_by_handle(
        &rf->texture_system, environment->source_cubemap);
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
  char irradiance_name_storage[128];
  char prefilter_name_storage[128];
  snprintf(source_name_storage, sizeof(source_name_storage),
           "__ibl.scene.%p.source", (void *)scene);
  snprintf(irradiance_name_storage, sizeof(irradiance_name_storage),
           "__ibl.scene.%p.irradiance", (void *)scene);
  snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
           "__ibl.scene.%p.prefilter", (void *)scene);
  const String8 source_name = string8_create_from_cstr(
      (const uint8_t *)source_name_storage, string_length(source_name_storage));
  const String8 irradiance_name =
      string8_create_from_cstr((const uint8_t *)irradiance_name_storage,
                               string_length(irradiance_name_storage));
  const String8 prefilter_name =
      string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                               string_length(prefilter_name_storage));

  if ((environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT &&
       !vkr_world_resources_create_writable_cube_texture(
           rf, source_name, environment->source_face_size, true_v,
           VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
           &environment->source_cubemap)) ||
      !vkr_world_resources_create_writable_cube_texture(
          rf, irradiance_name, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, false_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          &environment->irradiance_cubemap) ||
      !vkr_world_resources_create_writable_cube_texture(
          rf, prefilter_name, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE, true_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          &environment->prefilter_cubemap)) {
    goto failed;
  }

  const bool8_t baked =
      environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT
          ? rf->asset_publisher.bake_hdr_environment(
                rf->asset_publisher.state, environment->delivery_equirect,
                environment->source_cubemap, environment->irradiance_cubemap,
                environment->prefilter_cubemap)
          : rf->asset_publisher.bake_ibl_cubemap(
                rf->asset_publisher.state, environment->source_cubemap,
                environment->irradiance_cubemap,
                environment->prefilter_cubemap);
  if (!baked) {
    goto failed;
  }
  (void)vkr_world_resources_release_texture(&rf->texture_system,
                                            &environment->delivery_equirect);
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_READY;
  return true_v;

failed:
  vkr_world_resources_fail_scene_environment(rf, scene);
  return false_v;
}

bool8_t vkr_world_resources_prepare_scene_environment(
    RendererFrontend *rf, VkrWorldResources *resources, VkrScene *scene) {
  if (!rf || !resources || !scene || !scene->environment.enabled) {
    if (rf && scene) {
      vkr_world_resources_fail_scene_environment(rf, scene);
    } else if (scene) {
      scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
    }
    return false_v;
  }
  if (vkr_world_resources_has_retained_ibl_publisher(rf)) {
    return vkr_world_resources_prepare_published_environment(rf, resources,
                                                             scene);
  }
  if (!resources->supports_hdr_ibl || !resources->ibl_bake_runtime_ready) {
    vkr_world_resources_fail_scene_environment(rf, scene);
    return false_v;
  }

  VkrSceneEnvironment *environment = &scene->environment;
  VkrTexture *delivery = NULL;
  if (environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT) {
    delivery = vkr_texture_system_get_by_handle(&rf->texture_system,
                                                environment->delivery_equirect);
    if (!delivery || !delivery->handle ||
        delivery->description.type != VKR_TEXTURE_TYPE_2D ||
        delivery->description.format !=
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT ||
        !vkr_ibl_derive_cubemap_size(
            delivery->description.width, delivery->description.height,
            resources->hdr_ibl_max_cube_extent,
            vkr_world_resources_ibl_mip_limit(resources),
            &environment->source_face_size, &environment->source_mip_count)) {
      vkr_world_resources_fail_scene_environment(rf, scene);
      return false_v;
    }
  } else if (environment->source_kind == VKR_SCENE_ENV_SOURCE_CUBEMAP) {
    VkrTexture *source = vkr_texture_system_get_by_handle(
        &rf->texture_system, environment->source_cubemap);
    if (!source || !source->handle ||
        source->description.type != VKR_TEXTURE_TYPE_CUBE_MAP) {
      vkr_world_resources_fail_scene_environment(rf, scene);
      return false_v;
    }
    environment->source_face_size = source->description.width;
    environment->source_mip_count = 1u;
  } else {
    vkr_world_resources_fail_scene_environment(rf, scene);
    return false_v;
  }

  char source_name_storage[128];
  char irradiance_name_storage[128];
  char prefilter_name_storage[128];
  snprintf(source_name_storage, sizeof(source_name_storage),
           "__ibl.scene.%p.source", (void *)scene);
  snprintf(irradiance_name_storage, sizeof(irradiance_name_storage),
           "__ibl.scene.%p.irradiance", (void *)scene);
  snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
           "__ibl.scene.%p.prefilter", (void *)scene);
  String8 source_name = string8_create_from_cstr(
      (const uint8_t *)source_name_storage, string_length(source_name_storage));
  String8 irradiance_name =
      string8_create_from_cstr((const uint8_t *)irradiance_name_storage,
                               string_length(irradiance_name_storage));
  String8 prefilter_name =
      string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                               string_length(prefilter_name_storage));

  if (environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT &&
      !vkr_world_resources_create_writable_cube_texture(
          rf, source_name, environment->source_face_size, true_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          &environment->source_cubemap)) {
    goto cleanup;
  }
  if (!vkr_world_resources_create_writable_cube_texture(
          rf, irradiance_name, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, false_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          &environment->irradiance_cubemap) ||
      !vkr_world_resources_create_writable_cube_texture(
          rf, prefilter_name, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE, true_v,
          VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
          &environment->prefilter_cubemap)) {
    goto cleanup;
  }

  VkrTextureOpaqueHandle source_texture =
      vkr_world_resources_resolve_backend_texture(&rf->texture_system,
                                                  environment->source_cubemap,
                                                  VKR_TEXTURE_TYPE_CUBE_MAP);
  VkrTextureOpaqueHandle irradiance_texture =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, environment->irradiance_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP);
  VkrTextureOpaqueHandle prefilter_texture =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, environment->prefilter_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP);
  if (!source_texture || !irradiance_texture || !prefilter_texture ||
      (environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT &&
       !vkr_world_resources_prepare_target_set(
           rf, resources, source_texture, environment->source_face_size,
           environment->source_mip_count, &environment->cube_targets)) ||
      !vkr_world_resources_prepare_target_set(
          rf, resources, irradiance_texture,
          VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, 1u,
          &environment->irradiance_targets) ||
      !vkr_world_resources_prepare_target_set(
          rf, resources, prefilter_texture,
          VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE,
          vkr_world_resources_calculate_mip_count(
              VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE),
          &environment->prefilter_targets)) {
    goto cleanup;
  }

  environment->bake_state =
      environment->source_kind == VKR_SCENE_ENV_SOURCE_EQUIRECT
          ? VKR_SCENE_ENV_BAKE_STATE_CUBE_PENDING
          : VKR_SCENE_ENV_BAKE_STATE_CONVOLUTION_PENDING;
  return true_v;

cleanup:
  vkr_world_resources_fail_scene_environment(rf, scene);
  return false_v;
}

void vkr_world_resources_bake_scene_ibl_if_pending(RendererFrontend *rf,
                                                   VkrWorldResources *resources,
                                                   VkrScene *scene) {
  if (!rf || !resources || !scene) {
    return;
  }
  if (!scene->environment.enabled ||
      (scene->environment.bake_state != VKR_SCENE_ENV_BAKE_STATE_CUBE_PENDING &&
       scene->environment.bake_state !=
           VKR_SCENE_ENV_BAKE_STATE_CONVOLUTION_PENDING)) {
    return;
  }

  VkrSceneEnvironment *environment = &scene->environment;
  if (environment->bake_state == VKR_SCENE_ENV_BAKE_STATE_CUBE_PENDING) {
    VkrTextureOpaqueHandle delivery_texture =
        vkr_world_resources_resolve_backend_texture(
            &rf->texture_system, environment->delivery_equirect,
            VKR_TEXTURE_TYPE_2D);
    if (!delivery_texture ||
        vkr_renderer_image_barrier(rf, delivery_texture,
                                   VKR_IMAGE_ACCESS_TRANSFER_DST,
                                   VKR_IMAGE_ACCESS_SAMPLED,
                                   VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   NULL) != VKR_RENDERER_ERROR_NONE ||
        !vkr_world_resources_bake_cubemap(
            rf, resources, string8_lit("shader.ibl.equirect_to_cube"),
            resources->ibl_equirect_bake_shader_id,
            resources->ibl_equirect_bake_pipeline, "source_equirect",
            delivery_texture, &environment->cube_targets, false_v, 1u, 1u)) {
      vkr_world_resources_fail_scene_environment(rf, scene);
      return;
    }
    vkr_world_resources_destroy_target_set(rf, &environment->cube_targets);
    vkr_world_resources_release_texture(&rf->texture_system,
                                        &environment->delivery_equirect);
    environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_CONVOLUTION_PENDING;
  }

  VkrTextureOpaqueHandle source_texture =
      vkr_world_resources_resolve_backend_texture(&rf->texture_system,
                                                  environment->source_cubemap,
                                                  VKR_TEXTURE_TYPE_CUBE_MAP);
  if (!source_texture ||
      !vkr_world_resources_bake_cubemap(
          rf, resources, string8_lit("shader.ibl.diffuse_convolution"),
          resources->ibl_diffuse_bake_shader_id,
          resources->ibl_diffuse_bake_pipeline, "source_cubemap",
          source_texture, &environment->irradiance_targets, false_v,
          environment->source_face_size, environment->source_mip_count) ||
      !vkr_world_resources_bake_cubemap(
          rf, resources, string8_lit("shader.ibl.specular_prefilter"),
          resources->ibl_specular_bake_shader_id,
          resources->ibl_specular_bake_pipeline, "source_cubemap",
          source_texture, &environment->prefilter_targets, true_v,
          environment->source_face_size, environment->source_mip_count)) {
    vkr_world_resources_fail_scene_environment(rf, scene);
    return;
  }

  vkr_world_resources_destroy_target_set(rf, &environment->irradiance_targets);
  vkr_world_resources_destroy_target_set(rf, &environment->prefilter_targets);
  environment->bake_state = VKR_SCENE_ENV_BAKE_STATE_READY;
}

void vkr_world_resources_release_scene_reflection_probe_targets(
    RendererFrontend *rf, VkrScene *scene) {
  if (!rf || !scene) {
    return;
  }
  for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
    vkr_world_resources_destroy_target_set(
        rf, &scene->reflection_probes[i].irradiance_targets);
    vkr_world_resources_destroy_target_set(
        rf, &scene->reflection_probes[i].prefilter_targets);
  }
}

vkr_internal void
vkr_world_resources_fail_reflection_probe(RendererFrontend *rf,
                                          VkrSceneReflectionProbe *probe) {
  if (!rf || !probe) {
    return;
  }

  vkr_world_resources_destroy_target_set(rf, &probe->irradiance_targets);
  vkr_world_resources_destroy_target_set(rf, &probe->prefilter_targets);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &probe->irradiance_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &probe->prefilter_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &probe->source_cubemap);
  probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_FAILED;
}

vkr_internal bool8_t vkr_world_resources_retain_environment_probe_maps(
    RendererFrontend *rf, VkrScene *scene, VkrSceneReflectionProbe *probe) {
  if (!rf || !scene || !probe ||
      scene->environment.bake_state != VKR_SCENE_ENV_BAKE_STATE_READY ||
      !vkr_world_resources_texture_is_valid(
          &rf->texture_system, scene->environment.irradiance_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP) ||
      !vkr_world_resources_texture_is_valid(
          &rf->texture_system, scene->environment.prefilter_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP)) {
    return false_v;
  }

  vkr_texture_system_add_ref_by_handle(&rf->texture_system,
                                       scene->environment.irradiance_cubemap);
  vkr_texture_system_add_ref_by_handle(&rf->texture_system,
                                       scene->environment.prefilter_cubemap);
  probe->irradiance_cubemap = scene->environment.irradiance_cubemap;
  probe->prefilter_cubemap = scene->environment.prefilter_cubemap;
  probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY;
  return true_v;
}

bool8_t vkr_world_resources_prepare_scene_reflection_probes(
    RendererFrontend *rf, VkrWorldResources *resources, VkrScene *scene) {
  if (!rf || !resources || !scene) {
    return false_v;
  }
  if (vkr_world_resources_has_retained_ibl_publisher(rf)) {
    bool8_t all_prepared = true_v;
    for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
      VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
      if (!probe->enabled ||
          probe->bake_state != VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING) {
        continue;
      }

      if (probe->uses_scene_environment_source) {
        if (scene->environment.bake_state != VKR_SCENE_ENV_BAKE_STATE_READY) {
          if (scene->environment.bake_state ==
              VKR_SCENE_ENV_BAKE_STATE_FAILED) {
            vkr_world_resources_fail_reflection_probe(rf, probe);
            all_prepared = false_v;
          }
          continue;
        }
        if (!vkr_world_resources_retain_environment_probe_maps(rf, scene,
                                                               probe)) {
          vkr_world_resources_fail_reflection_probe(rf, probe);
          all_prepared = false_v;
        }
        continue;
      }

      char irradiance_name_storage[160];
      char prefilter_name_storage[160];
      snprintf(irradiance_name_storage, sizeof(irradiance_name_storage),
               "__ibl.scene.%p.probe.%u.irradiance", (void *)scene, i);
      snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
               "__ibl.scene.%p.probe.%u.prefilter", (void *)scene, i);
      const String8 irradiance_name =
          string8_create_from_cstr((const uint8_t *)irradiance_name_storage,
                                   string_length(irradiance_name_storage));
      const String8 prefilter_name =
          string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                                   string_length(prefilter_name_storage));
      if (!vkr_world_resources_create_writable_cube_texture(
              rf, irradiance_name, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE,
              false_v, VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
              &probe->irradiance_cubemap) ||
          !vkr_world_resources_create_writable_cube_texture(
              rf, prefilter_name, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE,
              true_v, VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
              &probe->prefilter_cubemap) ||
          !rf->asset_publisher.bake_ibl_cubemap(
              rf->asset_publisher.state, probe->source_cubemap,
              probe->irradiance_cubemap, probe->prefilter_cubemap)) {
        vkr_world_resources_fail_reflection_probe(rf, probe);
        all_prepared = false_v;
        continue;
      }
      probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY;
    }
    return all_prepared;
  }
  if (!resources->supports_hdr_ibl || !resources->ibl_bake_runtime_ready) {
    for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
      VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
      if (probe->bake_state == VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING) {
        vkr_world_resources_fail_reflection_probe(rf, probe);
      }
    }
    return false_v;
  }

  bool8_t all_prepared = true_v;
  for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
    VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
    if (!probe->enabled ||
        probe->bake_state != VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING) {
      continue;
    }

    // A probe sourced from the scene environment needs different spatial
    // controls, not a second convolution of identical texels. The environment
    // bake publishes retained map references once it becomes ready below.
    if (probe->uses_scene_environment_source) {
      continue;
    }

    char irradiance_name_storage[160];
    char prefilter_name_storage[160];
    snprintf(irradiance_name_storage, sizeof(irradiance_name_storage),
             "__ibl.scene.%p.probe.%u.irradiance", (void *)scene, i);
    snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
             "__ibl.scene.%p.probe.%u.prefilter", (void *)scene, i);
    String8 irradiance_name =
        string8_create_from_cstr((const uint8_t *)irradiance_name_storage,
                                 string_length(irradiance_name_storage));
    String8 prefilter_name =
        string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                                 string_length(prefilter_name_storage));

    if (!vkr_world_resources_create_writable_cube_texture(
            rf, irradiance_name, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE,
            false_v, VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
            &probe->irradiance_cubemap) ||
        !vkr_world_resources_create_writable_cube_texture(
            rf, prefilter_name, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE, true_v,
            VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
            &probe->prefilter_cubemap)) {
      goto probe_failed;
    }

    VkrTextureOpaqueHandle irradiance_texture =
        vkr_world_resources_resolve_backend_texture(&rf->texture_system,
                                                    probe->irradiance_cubemap,
                                                    VKR_TEXTURE_TYPE_CUBE_MAP);
    VkrTextureOpaqueHandle prefilter_texture =
        vkr_world_resources_resolve_backend_texture(&rf->texture_system,
                                                    probe->prefilter_cubemap,
                                                    VKR_TEXTURE_TYPE_CUBE_MAP);
    if (!irradiance_texture || !prefilter_texture ||
        !vkr_world_resources_prepare_target_set(
            rf, resources, irradiance_texture,
            VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, 1u,
            &probe->irradiance_targets) ||
        !vkr_world_resources_prepare_target_set(
            rf, resources, prefilter_texture,
            VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE,
            vkr_world_resources_calculate_mip_count(
                VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE),
            &probe->prefilter_targets)) {
      goto probe_failed;
    }
    continue;

  probe_failed:
    vkr_world_resources_fail_reflection_probe(rf, probe);
    all_prepared = false_v;
  }
  return all_prepared;
}

void vkr_world_resources_bake_scene_reflection_probes_if_pending(
    RendererFrontend *rf, VkrWorldResources *resources, VkrScene *scene) {
  if (!rf || !resources || !scene || scene->reflection_probe_count == 0) {
    return;
  }
  uint32_t baked_ready_count = 0;
  uint32_t baked_failed_count = 0;

  for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
    VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
    if (!probe->enabled ||
        probe->bake_state != VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING) {
      continue;
    }

    if (probe->uses_scene_environment_source) {
      if (scene->environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_FAILED) {
        vkr_world_resources_fail_reflection_probe(rf, probe);
        baked_failed_count++;
        continue;
      }
      if (scene->environment.bake_state != VKR_SCENE_ENV_BAKE_STATE_READY) {
        continue;
      }

      if (!vkr_world_resources_retain_environment_probe_maps(rf, scene,
                                                             probe)) {
        vkr_world_resources_fail_reflection_probe(rf, probe);
        baked_failed_count++;
        continue;
      }
      baked_ready_count++;
      continue;
    }

    VkrTexture *source = vkr_texture_system_get_by_handle(
        &rf->texture_system, probe->source_cubemap);
    if (!source || !source->handle ||
        source->description.type != VKR_TEXTURE_TYPE_CUBE_MAP ||
        !probe->irradiance_targets.ready || !probe->prefilter_targets.ready) {
      vkr_world_resources_fail_reflection_probe(rf, probe);
      baked_failed_count++;
      continue;
    }

    bool8_t baked =
        vkr_world_resources_bake_cubemap(
            rf, resources, string8_lit("shader.ibl.diffuse_convolution"),
            resources->ibl_diffuse_bake_shader_id,
            resources->ibl_diffuse_bake_pipeline, "source_cubemap",
            source->handle, &probe->irradiance_targets, false_v,
            source->description.width, Max(1u, probe->source_mip_count)) &&
        vkr_world_resources_bake_cubemap(
            rf, resources, string8_lit("shader.ibl.specular_prefilter"),
            resources->ibl_specular_bake_shader_id,
            resources->ibl_specular_bake_pipeline, "source_cubemap",
            source->handle, &probe->prefilter_targets, true_v,
            source->description.width, Max(1u, probe->source_mip_count));

    if (!baked) {
      vkr_world_resources_fail_reflection_probe(rf, probe);
      baked_failed_count++;
      continue;
    }

    vkr_world_resources_destroy_target_set(rf, &probe->irradiance_targets);
    vkr_world_resources_destroy_target_set(rf, &probe->prefilter_targets);
    probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY;
    baked_ready_count++;
  }

  if (baked_ready_count > 0 || baked_failed_count > 0) {
    log_info("World resources: reflection probe bake finished (ready=%u, "
             "failed=%u, total=%u)",
             baked_ready_count, baked_failed_count,
             scene->reflection_probe_count);
  }
}

float32_t vkr_world_resources_probe_fragment_influence(Vec3 center,
                                                       Vec3 extents,
                                                       float32_t blend_distance,
                                                       Vec3 world_position) {
  float32_t dx = vkr_abs_f32(world_position.x - center.x);
  float32_t dy = vkr_abs_f32(world_position.y - center.y);
  float32_t dz = vkr_abs_f32(world_position.z - center.z);
  float32_t outside_x = vkr_max_f32(0.0f, dx - extents.x);
  float32_t outside_y = vkr_max_f32(0.0f, dy - extents.y);
  float32_t outside_z = vkr_max_f32(0.0f, dz - extents.z);
  float32_t outside_distance = vkr_sqrt_f32(
      outside_x * outside_x + outside_y * outside_y + outside_z * outside_z);

  if (outside_distance <= 1e-6f) {
    return 1.0f;
  }
  if (blend_distance <= 0.0f) {
    return 0.0f;
  }

  return vkr_max_f32(0.0f, 1.0f - outside_distance / blend_distance);
}

bool8_t vkr_world_resources_probe_intersects_sphere(Vec3 center, Vec3 extents,
                                                    float32_t blend_distance,
                                                    Vec3 sphere_center,
                                                    float32_t sphere_radius) {
  Vec3 influence_extents = vec3_add(
      extents, vec3_new(blend_distance, blend_distance, blend_distance));
  Vec3 delta = vec3_new(vkr_abs_f32(sphere_center.x - center.x),
                        vkr_abs_f32(sphere_center.y - center.y),
                        vkr_abs_f32(sphere_center.z - center.z));
  Vec3 outside = vec3_new(vkr_max_f32(0.0f, delta.x - influence_extents.x),
                          vkr_max_f32(0.0f, delta.y - influence_extents.y),
                          vkr_max_f32(0.0f, delta.z - influence_extents.z));
  float32_t radius = vkr_max_f32(sphere_radius, 0.0f);
  return vec3_length_squared(outside) <= radius * radius ? true_v : false_v;
}

vkr_internal VkrWorldIblProbeSlot vkr_world_resources_fallback_probe_slot(
    RendererFrontend *rf, VkrWorldResources *resources) {
  VkrWorldIblProbeSlot slot = {
      .irradiance_map = NULL,
      .prefilter_map = NULL,
      .center = {0},
      .extents = {0},
      .blend_distance = 0.0f,
      .weight = 1.0f,
      .intensity = 1.0f,
      .diffuse_intensity = 1.0f,
      .specular_intensity = 1.0f,
      .box_projection_enabled = false_v,
  };

  if (!rf || !resources) {
    return slot;
  }

  slot.irradiance_map = vkr_world_resources_resolve_backend_texture(
      &rf->texture_system, resources->ibl_active_irradiance_cubemap,
      VKR_TEXTURE_TYPE_CUBE_MAP);
  slot.prefilter_map = vkr_world_resources_resolve_backend_texture(
      &rf->texture_system, resources->ibl_active_prefilter_cubemap,
      VKR_TEXTURE_TYPE_CUBE_MAP);
  if (!slot.irradiance_map) {
    slot.irradiance_map = vkr_world_resources_resolve_backend_texture(
        &rf->texture_system, resources->ibl_fallback_irradiance_cubemap,
        VKR_TEXTURE_TYPE_CUBE_MAP);
  }
  if (!slot.prefilter_map) {
    slot.prefilter_map = vkr_world_resources_resolve_backend_texture(
        &rf->texture_system, resources->ibl_fallback_prefilter_cubemap,
        VKR_TEXTURE_TYPE_CUBE_MAP);
  }
  return slot;
}

void vkr_world_resources_select_probe_slots_for_position(
    RendererFrontend *rf, VkrWorldResources *resources, const VkrScene *scene,
    Vec3 world_position, VkrWorldIblProbeSlot out_slots[3]) {
  vkr_world_resources_select_probe_slots_for_bounds(
      rf, resources, scene, world_position, 0.0f, out_slots);
}

void vkr_world_resources_select_probe_slots_for_bounds(
    RendererFrontend *rf, VkrWorldResources *resources, const VkrScene *scene,
    Vec3 bounds_center, float32_t bounds_radius,
    VkrWorldIblProbeSlot out_slots[3]) {
  if (!out_slots) {
    return;
  }

  MemZero(out_slots, sizeof(VkrWorldIblProbeSlot) * 3u);
  if (!rf || !resources) {
    out_slots[2].weight = 1.0f;
    return;
  }

  if (!resources->ibl_default_ready) {
    if (!vkr_world_resources_ensure_default_ibl_ready(rf, resources)) {
      out_slots[2].weight = 1.0f;
      return;
    }
  }

  VkrWorldIblProbeSlot fallback =
      vkr_world_resources_fallback_probe_slot(rf, resources);
  out_slots[0] = fallback;
  out_slots[1] = fallback;
  out_slots[2] = fallback;
  out_slots[0].weight = 0.0f;
  out_slots[1].weight = 0.0f;
  out_slots[2].weight = 1.0f;

  uint32_t best_probe_index[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
  float32_t best_probe_score[2] = {-VKR_FLOAT_MAX, -VKR_FLOAT_MAX};
  if (scene) {
    for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
      const VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
      if (!probe->enabled ||
          probe->bake_state != VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_READY) {
        continue;
      }

      VkrTextureOpaqueHandle irradiance =
          vkr_world_resources_resolve_backend_texture(
              &rf->texture_system, probe->irradiance_cubemap,
              VKR_TEXTURE_TYPE_CUBE_MAP);
      VkrTextureOpaqueHandle prefilter =
          vkr_world_resources_resolve_backend_texture(
              &rf->texture_system, probe->prefilter_cubemap,
              VKR_TEXTURE_TYPE_CUBE_MAP);
      if (!irradiance || !prefilter) {
        continue;
      }

      if (!vkr_world_resources_probe_intersects_sphere(
              probe->center, probe->extents, probe->blend_distance,
              bounds_center, bounds_radius)) {
        continue;
      }

      float32_t center_distance =
          vec3_length(vec3_sub(bounds_center, probe->center));
      float32_t score = -center_distance;
      if (score > best_probe_score[0]) {
        best_probe_score[1] = best_probe_score[0];
        best_probe_index[1] = best_probe_index[0];
        best_probe_score[0] = score;
        best_probe_index[0] = i;
      } else if (score > best_probe_score[1]) {
        best_probe_score[1] = score;
        best_probe_index[1] = i;
      }
    }
  }

  for (uint32_t slot_index = 0; slot_index < 2u; ++slot_index) {
    if (best_probe_index[slot_index] == 0xFFFFFFFFu) {
      continue;
    }
    const VkrSceneReflectionProbe *probe =
        &scene->reflection_probes[best_probe_index[slot_index]];
    out_slots[slot_index] = (VkrWorldIblProbeSlot){
        .irradiance_map = vkr_world_resources_resolve_backend_texture(
            &rf->texture_system, probe->irradiance_cubemap,
            VKR_TEXTURE_TYPE_CUBE_MAP),
        .prefilter_map = vkr_world_resources_resolve_backend_texture(
            &rf->texture_system, probe->prefilter_cubemap,
            VKR_TEXTURE_TYPE_CUBE_MAP),
        .center = probe->center,
        .extents = probe->extents,
        .blend_distance = probe->blend_distance,
        .weight = 1.0f,
        .intensity = probe->intensity,
        .diffuse_intensity = probe->diffuse_intensity,
        .specular_intensity = probe->specular_intensity,
        .box_projection_enabled = true_v,
    };
  }
}

void vkr_world_resources_set_active_ibl_from_scene_or_default(
    RendererFrontend *rf, VkrWorldResources *resources, const VkrScene *scene) {
  if (!rf || !resources) {
    return;
  }

  if (!vkr_world_resources_ensure_default_ibl_ready(rf, resources)) {
    resources->ibl_active_enabled = false_v;
    resources->ibl_active_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
    resources->ibl_active_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
    return;
  }

  bool8_t use_scene_ibl =
      scene && scene->environment.enabled &&
      scene->environment.bake_state == VKR_SCENE_ENV_BAKE_STATE_READY &&
      vkr_world_resources_texture_is_valid(
          &rf->texture_system, scene->environment.irradiance_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP) &&
      vkr_world_resources_texture_is_valid(&rf->texture_system,
                                           scene->environment.prefilter_cubemap,
                                           VKR_TEXTURE_TYPE_CUBE_MAP);

  if (use_scene_ibl) {
    resources->ibl_active_irradiance_cubemap =
        scene->environment.irradiance_cubemap;
    resources->ibl_active_prefilter_cubemap =
        scene->environment.prefilter_cubemap;
    resources->ibl_active_enabled = true_v;
    resources->ibl_active_intensity = scene->environment.intensity;
    resources->ibl_active_diffuse_intensity =
        scene->environment.diffuse_intensity;
    resources->ibl_active_specular_intensity =
        scene->environment.specular_intensity;
    return;
  }

  resources->ibl_active_irradiance_cubemap =
      resources->ibl_fallback_irradiance_cubemap;
  resources->ibl_active_prefilter_cubemap =
      resources->ibl_fallback_prefilter_cubemap;
  resources->ibl_active_enabled = true_v;
  resources->ibl_active_intensity = 1.0f;
  resources->ibl_active_diffuse_intensity = 1.0f;
  resources->ibl_active_specular_intensity = 1.0f;
}

void vkr_world_resources_apply_active_ibl_to_material_system(
    RendererFrontend *rf, VkrWorldResources *resources) {
  if (!rf || !resources) {
    return;
  }

  VkrTextureOpaqueHandle irradiance =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, resources->ibl_active_irradiance_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP);
  VkrTextureOpaqueHandle prefilter =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, resources->ibl_active_prefilter_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP);
  VkrTextureOpaqueHandle brdf = vkr_world_resources_resolve_backend_texture(
      &rf->texture_system, resources->ibl_brdf_lut, VKR_TEXTURE_TYPE_2D);

  if (!irradiance) {
    irradiance = vkr_world_resources_resolve_backend_texture(
        &rf->texture_system, resources->ibl_fallback_irradiance_cubemap,
        VKR_TEXTURE_TYPE_CUBE_MAP);
  }
  if (!prefilter) {
    prefilter = vkr_world_resources_resolve_backend_texture(
        &rf->texture_system, resources->ibl_fallback_prefilter_cubemap,
        VKR_TEXTURE_TYPE_CUBE_MAP);
  }
  if (!brdf) {
    VkrTextureHandle fallback_specular =
        vkr_texture_system_get_default_specular_handle(&rf->texture_system);
    brdf = vkr_world_resources_resolve_backend_texture(
        &rf->texture_system, fallback_specular, VKR_TEXTURE_TYPE_2D);
  }

  bool8_t enabled =
      resources->ibl_active_enabled && irradiance && prefilter && brdf;
  vkr_material_system_set_ibl_maps(
      &rf->material_system, irradiance, prefilter, brdf, enabled,
      resources->ibl_active_intensity, resources->ibl_active_diffuse_intensity,
      resources->ibl_active_specular_intensity);
}

VkrRendererError vkr_world_resources_record_tonemap(
    RendererFrontend *rf, VkrWorldResources *resources,
    VkrTextureOpaqueHandle source_hdr, uint32_t width, uint32_t height,
    float32_t exposure) {
  if (!rf || !resources || width == 0u || height == 0u) {
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  }
  if (!source_hdr || resources->tonemap_shader_id == 0u ||
      resources->tonemap_pipeline.id == 0 ||
      resources->tonemap_instance_state.id == VKR_INVALID_ID ||
      !vkr_shader_system_use_by_id(&rf->shader_system,
                                   resources->tonemap_shader_id)) {
    return VKR_RENDERER_ERROR_INVALID_HANDLE;
  }

  VkrRendererError bind_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_bind_pipeline(
          &rf->pipeline_registry, resources->tonemap_pipeline, &bind_error)) {
    return bind_error != VKR_RENDERER_ERROR_NONE
               ? bind_error
               : VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
  }
  if (!vkr_shader_system_uniform_set(&rf->shader_system, "exposure",
                                     &exposure) ||
      !vkr_shader_system_apply_global(&rf->shader_system) ||
      !vkr_shader_system_bind_instance(&rf->shader_system,
                                       resources->tonemap_instance_state.id) ||
      !vkr_shader_system_sampler_set(&rf->shader_system, "source_hdr",
                                     source_hdr) ||
      !vkr_shader_system_apply_instance(&rf->shader_system)) {
    return VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED;
  }

  VkrViewport viewport = {.x = 0.0f,
                          .y = 0.0f,
                          .width = (float32_t)width,
                          .height = (float32_t)height,
                          .min_depth = 0.0f,
                          .max_depth = 1.0f};
  VkrScissor scissor = {.x = 0, .y = 0, .width = width, .height = height};
  vkr_renderer_set_viewport(rf, &viewport);
  vkr_renderer_set_scissor(rf, &scissor);
  vkr_renderer_draw(rf, 3u, 1u, 0u, 0u);
  return VKR_RENDERER_ERROR_NONE;
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

  vkr_world_resources_destroy_target_set(
      rf, &resources->ibl_default_prefilter_targets);
  vkr_world_resources_destroy_target_set(
      rf, &resources->ibl_default_irradiance_targets);
  vkr_world_resources_destroy_target_set(
      rf, &resources->ibl_default_source_targets);
  if (resources->ibl_brdf_bake_target) {
    vkr_renderer_render_target_destroy(rf, resources->ibl_brdf_bake_target);
    resources->ibl_brdf_bake_target = NULL;
  }

  vkr_world_resources_release_texture(
      &rf->texture_system, &resources->ibl_fallback_prefilter_cubemap);
  vkr_world_resources_release_texture(
      &rf->texture_system, &resources->ibl_fallback_irradiance_cubemap);
  const bool8_t legacy_aliases_active_source =
      resources->ibl_legacy_fallback_source_cubemap.id ==
          resources->ibl_fallback_source_cubemap.id &&
      resources->ibl_legacy_fallback_source_cubemap.generation ==
          resources->ibl_fallback_source_cubemap.generation;
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &resources->ibl_fallback_source_cubemap);
  vkr_world_resources_release_texture(
      &rf->texture_system, &resources->ibl_default_delivery_equirect);
  if (resources->ibl_legacy_fallback_source_cubemap.id != 0 &&
      !legacy_aliases_active_source) {
    vkr_world_resources_release_texture(
        &rf->texture_system, &resources->ibl_legacy_fallback_source_cubemap);
  } else {
    resources->ibl_legacy_fallback_source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  }
  if (resources->ibl_brdf_lut.id != 0) {
    VkrTextureHandle default_specular =
        vkr_texture_system_get_default_specular_handle(&rf->texture_system);
    if (resources->ibl_brdf_lut.id != default_specular.id ||
        resources->ibl_brdf_lut.generation != default_specular.generation) {
      vkr_world_resources_release_texture(&rf->texture_system,
                                          &resources->ibl_brdf_lut);
    } else {
      resources->ibl_brdf_lut = VKR_TEXTURE_HANDLE_INVALID;
    }
  }

  vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
  vkr_world_resources_destroy_tonemap_runtime(rf, resources);

  if (resources->text_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->text_pipeline);
    resources->text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (resources->pbr_overlay_double_sided_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(
        &rf->pipeline_registry, resources->pbr_overlay_double_sided_pipeline);
    resources->pbr_overlay_double_sided_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_transparent_double_sided_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(
        &rf->pipeline_registry,
        resources->pbr_transparent_double_sided_pipeline);
    resources->pbr_transparent_double_sided_pipeline =
        VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_double_sided_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(
        &rf->pipeline_registry, resources->pbr_double_sided_pipeline);
    resources->pbr_double_sided_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (resources->overlay_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->overlay_pipeline);
    resources->overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (resources->pbr_overlay_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->pbr_overlay_pipeline);
    resources->pbr_overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_transparent_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->pbr_transparent_pipeline);
    resources->pbr_transparent_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }
  if (resources->pbr_pipeline.id != 0) {
    vkr_pipeline_registry_destroy_pipeline(&rf->pipeline_registry,
                                           resources->pbr_pipeline);
    resources->pbr_pipeline = VKR_PIPELINE_HANDLE_INVALID;
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

  resources->ibl_active_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_enabled = false_v;
  resources->ibl_active_intensity = 1.0f;
  resources->ibl_active_diffuse_intensity = 1.0f;
  resources->ibl_active_specular_intensity = 1.0f;
  resources->ibl_bake_runtime_ready = false_v;
  resources->ibl_bake_render_pass_owned = false_v;
  resources->ibl_default_ready = false_v;
  resources->ibl_default_prepared = false_v;
  resources->ibl_brdf_baked = false_v;
  resources->ibl_default_cube_baked = false_v;

  resources->initialized = false_v;
}

bool8_t vkr_world_resources_text_create(RendererFrontend *rf,
                                        VkrWorldResources *resources,
                                        const VkrWorldTextCreateData *payload) {
  if (!rf || !resources || !payload) {
    return false_v;
  }

  if (rf->impl.caps.uses_legacy_pipeline_state &&
      resources->text_pipeline.id == 0) {
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

  VkrWorldTextSlot *slot =
      vkr_world_resources_get_text_slot(resources, text_id);
  if (!slot) {
    log_warn("World text id %u not found for update", text_id);
    return false_v;
  }

  vkr_text_3d_set_text(&slot->text, content);
  return true_v;
}

bool8_t vkr_world_resources_text_set_transform(RendererFrontend *rf,
                                               VkrWorldResources *resources,
                                               uint32_t text_id,
                                               const VkrTransform *transform) {
  (void)rf;
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

bool8_t vkr_world_resources_text_destroy(RendererFrontend *rf,
                                         VkrWorldResources *resources,
                                         uint32_t text_id) {
  (void)rf;
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

uint32_t vkr_world_resources_prepare_text_draws(RendererFrontend *rf,
                                                VkrWorldResources *resources,
                                                VkrPreparedTextDraw *out_draws,
                                                uint32_t capacity) {
  if (!rf || !resources || !out_draws || capacity == 0)
    return 0;
  uint32_t count = 0;
  for (uint64_t i = 0; i < resources->text_slots.length; ++i) {
    VkrWorldTextSlot *slot = &resources->text_slots.data[i];
    VkrText3D *text = &slot->text;
    if (!slot->active || !vkr_text_3d_prepare_geometry(text) ||
        text->index_count == 0)
      continue;
    if (count == capacity) {
      log_error("World text packet capacity exceeded (%u)", capacity);
      break;
    }
    VkrFont *font =
        vkr_font_system_get_by_handle(text->font_system, text->font);
    if (!font)
      font = vkr_font_system_get_default_mtsdf_font(text->font_system);
    if (!font || font->atlas.id == 0 ||
        font->atlas.generation == VKR_INVALID_ID)
      continue;
    float32_t screen_px_range = 0.0f;
    uint32_t font_mode = 0;
    if (font->type == VKR_FONT_TYPE_MTSDF && font->em_size > 0.0f) {
      const float32_t render_size =
          text->font_size > 0.0f ? text->font_size : (float32_t)font->size;
      font_mode = 1;
      screen_px_range = Clamp(
          font->sdf_distance_range * (render_size / font->em_size), 1.0f, 4.0f);
    }
    Mat4 model = vkr_transform_get_world(&text->transform);
    if (text->texture_width > 0 && text->texture_height > 0) {
      model = mat4_mul(
          model, mat4_scale(vec3_new(text->world_width / text->texture_width,
                                     text->world_height / text->texture_height,
                                     1.0f)));
    }
    out_draws[count++] = (VkrPreparedTextDraw){
        .vertices = text->vertices,
        .vertex_count = text->vertex_count,
        .indices = text->indices,
        .index_count = text->index_count,
        .max_index = text->vertex_count - 1u,
        .atlas = font->atlas,
        .model = model,
        .screen_px_range = screen_px_range,
        .font_mode = font_mode,
        .object_id =
            vkr_picking_encode_id(VKR_PICKING_ID_KIND_WORLD_TEXT, (uint32_t)i),
        .revision = text->geometry_revision,
    };
  }
  return count;
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
