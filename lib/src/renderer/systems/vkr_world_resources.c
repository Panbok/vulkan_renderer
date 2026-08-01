/**
 * @file vkr_world_resources.c
 * @brief Stateless world pipelines and 3D text resources.
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

#define VKR_WORLD_RESOURCES_MAX_TEXTS 16
#define VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE 64u
#define VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE 256u
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

vkr_internal void
vkr_world_resources_release_texture(VkrTextureSystem *texture_system,
                                    VkrTextureHandle *handle) {
  if (!texture_system || !handle || handle->id == 0) {
    return;
  }

  vkr_texture_system_release_by_handle(texture_system, *handle);
  *handle = VKR_TEXTURE_HANDLE_INVALID;
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

vkr_internal uint32_t vkr_world_resources_calculate_mip_count(uint32_t size) {
  uint32_t mips = 1u;
  while (size > 1u) {
    size >>= 1u;
    mips++;
  }
  return mips;
}

vkr_internal bool8_t vkr_world_resources_create_writable_cube_texture(
    RendererFrontend *rf, String8 name, uint32_t size, bool8_t with_mips,
    VkrTextureHandle *out_handle) {
  if (!rf || !name.str || !out_handle || size == 0) {
    return false_v;
  }

  VkrTextureDescription desc = {
      .width = size,
      .height = size,
      .channels = 4,
      .type = VKR_TEXTURE_TYPE_CUBE_MAP,
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
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

vkr_internal Mat4 vkr_world_resources_ibl_capture_view(uint32_t face) {
  static const Vec3 k_face_targets[6] = {
      {1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
      {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f},
  };
  static const Vec3 k_face_ups[6] = {
      {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
  };
  const uint32_t safe_face = (face < 6u) ? face : 0u;
  return mat4_look_at(vec3_zero(), k_face_targets[safe_face],
                      k_face_ups[safe_face]);
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
      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
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

  vkr_world_resources_release_instance_state(
      rf, resources->ibl_diffuse_bake_pipeline,
      &resources->ibl_diffuse_bake_instance_state);
  vkr_world_resources_release_instance_state(
      rf, resources->ibl_specular_bake_pipeline,
      &resources->ibl_specular_bake_instance_state);

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

  if (resources->ibl_bake_cube_geometry.id != 0) {
    vkr_geometry_system_release(&rf->geometry_system,
                                resources->ibl_bake_cube_geometry);
    resources->ibl_bake_cube_geometry = (VkrGeometryHandle){0};
  }

  if (resources->ibl_bake_render_pass &&
      resources->ibl_bake_render_pass_owned) {
    vkr_renderer_renderpass_destroy(rf, resources->ibl_bake_render_pass);
  }
  resources->ibl_bake_render_pass = NULL;
  resources->ibl_bake_render_pass_owned = false_v;
  resources->ibl_bake_runtime_ready = false_v;
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
  resources->ibl_bake_cube_geometry = vkr_geometry_system_acquire_by_name(
      &rf->geometry_system, string8_lit("IBL Bake Cube"), false_v, &geom_err);
  if (resources->ibl_bake_cube_geometry.id == 0) {
    resources->ibl_bake_cube_geometry = vkr_geometry_system_create_cube(
        &rf->geometry_system, 2.0f, 2.0f, 2.0f, "IBL Bake Cube", &geom_err);
  }
  if (resources->ibl_bake_cube_geometry.id == 0) {
    String8 err = vkr_renderer_get_error_string(geom_err);
    log_warn("World resources: failed to create IBL bake cube geometry: %s",
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
  resources->ibl_specular_bake_shader_config =
      *(VkrShaderConfig *)specular_cfg_info.as.custom;

  const VkrShaderConfig *diffuse_cfg =
      &resources->ibl_diffuse_bake_shader_config;
  const VkrShaderConfig *specular_cfg =
      &resources->ibl_specular_bake_shader_config;
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

  resources->ibl_diffuse_bake_instance_state.id = VKR_INVALID_ID;
  resources->ibl_specular_bake_instance_state.id = VKR_INVALID_ID;

  VkrRendererError diffuse_instance_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, resources->ibl_diffuse_bake_pipeline,
          &resources->ibl_diffuse_bake_instance_state, &diffuse_instance_err)) {
    String8 err = vkr_renderer_get_error_string(diffuse_instance_err);
    log_warn(
        "World resources: failed to acquire diffuse bake instance state: %s",
        string8_cstr(&err));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  VkrRendererError specular_instance_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, resources->ibl_specular_bake_pipeline,
          &resources->ibl_specular_bake_instance_state,
          &specular_instance_err)) {
    String8 err = vkr_renderer_get_error_string(specular_instance_err);
    log_warn(
        "World resources: failed to acquire specular bake instance state: %s",
        string8_cstr(&err));
    vkr_world_resources_destroy_ibl_bake_runtime(rf, resources);
    return false_v;
  }

  resources->ibl_bake_runtime_ready = true_v;
  return true_v;
}

/**
 * @brief Renders a cubemap bake shader into all target faces (and mips).
 *
 * Each face/mip is rendered through a dedicated one-layer render target so the
 * renderpass layout transitions apply to only the written subresource.
 */
vkr_internal bool8_t vkr_world_resources_bake_cubemap(
    RendererFrontend *rf, VkrWorldResources *resources, const char *shader_name,
    VkrPipelineHandle pipeline, VkrRendererInstanceStateHandle instance_state,
    VkrTextureOpaqueHandle source_cubemap,
    VkrTextureOpaqueHandle target_cubemap, uint32_t base_size,
    uint32_t mip_count, bool8_t use_roughness_uniform) {
  if (!rf || !resources || !shader_name || pipeline.id == 0 ||
      instance_state.id == VKR_INVALID_ID || !source_cubemap ||
      !target_cubemap || base_size == 0 || mip_count == 0 ||
      !resources->ibl_bake_render_pass) {
    return false_v;
  }

  const Mat4 projection =
      mat4_perspective(vkr_to_radians(90.0f), 1.0f, 0.1f, 10.0f);
  for (uint32_t mip = 0; mip < mip_count; ++mip) {
    const uint32_t mip_size = Max(1u, base_size >> mip);
    const float32_t roughness =
        (mip_count > 1u) ? (float32_t)mip / (float32_t)(mip_count - 1u) : 0.0f;
    for (uint32_t face = 0; face < 6u; ++face) {
      VkrRenderTargetAttachmentRef attachment = {
          .texture = target_cubemap,
          .mip_level = mip,
          .base_layer = face,
          .layer_count = 1,
      };
      VkrRenderTargetDesc target_desc = {
          .sync_to_window_size = false_v,
          .attachment_count = 1,
          .attachments = &attachment,
          .width = mip_size,
          .height = mip_size,
      };

      VkrRendererError target_err = VKR_RENDERER_ERROR_NONE;
      VkrRenderTargetHandle render_target = vkr_renderer_render_target_create(
          rf, &target_desc, resources->ibl_bake_render_pass, &target_err);
      if (!render_target) {
        String8 err = vkr_renderer_get_error_string(target_err);
        log_warn("World resources: IBL bake render target creation failed: %s",
                 string8_cstr(&err));
        return false_v;
      }

      bool8_t baked = false_v;
      bool8_t began_pass = false_v;
      VkrRendererError begin_err = vkr_renderer_begin_render_pass(
          rf, resources->ibl_bake_render_pass, render_target);
      if (begin_err == VKR_RENDERER_ERROR_NONE) {
        began_pass = true_v;
      }
      if (began_pass &&
          vkr_shader_system_use(&rf->shader_system, shader_name)) {
        VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
        if (vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry,
                                                pipeline, &bind_err)) {
          Mat4 view = vkr_world_resources_ibl_capture_view(face);
          vkr_shader_system_uniform_set(&rf->shader_system, "projection",
                                        &projection);
          vkr_shader_system_uniform_set(&rf->shader_system, "view", &view);
          if (vkr_shader_system_apply_global(&rf->shader_system) &&
              vkr_shader_system_bind_instance(&rf->shader_system,
                                              instance_state.id) &&
              vkr_shader_system_sampler_set(&rf->shader_system,
                                            "source_cubemap", source_cubemap)) {
            if (!use_roughness_uniform ||
                vkr_shader_system_uniform_set(&rf->shader_system, "roughness",
                                              &roughness)) {
              if (vkr_shader_system_apply_instance(&rf->shader_system)) {
                VkrViewport viewport = {
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = (float32_t)mip_size,
                    .height = (float32_t)mip_size,
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                };
                VkrScissor scissor = {
                    .x = 0,
                    .y = 0,
                    .width = mip_size,
                    .height = mip_size,
                };
                vkr_renderer_set_viewport(rf, &viewport);
                vkr_renderer_set_scissor(rf, &scissor);
                vkr_geometry_system_render(rf, &rf->geometry_system,
                                           resources->ibl_bake_cube_geometry,
                                           1);
                baked = true_v;
              }
            }
          }
        }
      }

      if (began_pass) {
        vkr_renderer_end_render_pass(rf);
      }
      vkr_renderer_render_target_destroy(rf, render_target);
      if (!baked) {
        return false_v;
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
  VkrRendererError barrier_err = vkr_renderer_image_barrier(
      rf, target_cubemap, VKR_IMAGE_ACCESS_COLOR_ATTACHMENT,
      VKR_IMAGE_ACCESS_SAMPLED, VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, NULL);
  if (barrier_err != VKR_RENDERER_ERROR_NONE) {
    String8 err = vkr_renderer_get_error_string(barrier_err);
    log_error(
        "IBL bake: failed to make '%s' output visible to shader reads: %s",
        shader_name, string8_cstr(&err));
    return false_v;
  }

  return true_v;
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

  resources->ibl_fallback_source_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_brdf_lut = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_bake_render_pass = NULL;
  resources->ibl_diffuse_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->ibl_specular_bake_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  resources->ibl_diffuse_bake_instance_state.id = VKR_INVALID_ID;
  resources->ibl_specular_bake_instance_state.id = VKR_INVALID_ID;
  resources->ibl_bake_cube_geometry = (VkrGeometryHandle){0};
  resources->ibl_active_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_active_enabled = false_v;
  resources->ibl_active_intensity = 1.0f;
  resources->ibl_active_diffuse_intensity = 1.0f;
  resources->ibl_active_specular_intensity = 1.0f;
  resources->ibl_bake_runtime_ready = false_v;
  resources->ibl_bake_render_pass_owned = false_v;
  resources->ibl_default_ready = false_v;

  resources->initialized = true_v;
  return true_v;

cleanup:
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

bool8_t
vkr_world_resources_ensure_default_ibl_ready(RendererFrontend *rf,
                                             VkrWorldResources *resources) {
  if (!rf || !resources) {
    return false_v;
  }
  if (resources->ibl_default_ready) {
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

  VkrTextureHandle brdf_lut = VKR_TEXTURE_HANDLE_INVALID;
  VkrRendererError brdf_error = VKR_RENDERER_ERROR_NONE;
  if (!vkr_texture_system_load(
          &rf->texture_system,
          string8_lit(
              "assets/textures/ibl_brdf_lut.png?tc=data_mask&cs=linear"),
          &brdf_lut, &brdf_error)) {
    brdf_lut =
        vkr_texture_system_get_default_specular_handle(&rf->texture_system);
    log_warn(
        "World resources: BRDF LUT missing, using default specular texture");
  } else {
    /*
     * vkr_texture_system_load() registers 2D textures with ref_count=0 so the
     * caller must take ownership explicitly if it plans to release by handle.
     */
    vkr_texture_system_add_ref_by_handle(&rf->texture_system, brdf_lut);
  }

  resources->ibl_fallback_source_cubemap = fallback_source;
  resources->ibl_brdf_lut = brdf_lut;
  resources->ibl_fallback_irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  resources->ibl_fallback_prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;

  bool8_t baked_fallback = false_v;
  if (vkr_world_resources_ensure_ibl_bake_runtime_ready(rf, resources)) {
    VkrTextureHandle irradiance = VKR_TEXTURE_HANDLE_INVALID;
    VkrTextureHandle prefilter = VKR_TEXTURE_HANDLE_INVALID;
    if (vkr_world_resources_create_writable_cube_texture(
            rf, string8_lit("__ibl.default.irradiance"),
            VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, false_v, &irradiance) &&
        vkr_world_resources_create_writable_cube_texture(
            rf, string8_lit("__ibl.default.prefilter"),
            VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE, true_v, &prefilter)) {
      VkrTextureOpaqueHandle source_opaque =
          vkr_world_resources_resolve_backend_texture(
              &rf->texture_system, fallback_source, VKR_TEXTURE_TYPE_CUBE_MAP);
      VkrTextureOpaqueHandle irradiance_opaque =
          vkr_world_resources_resolve_backend_texture(
              &rf->texture_system, irradiance, VKR_TEXTURE_TYPE_CUBE_MAP);
      VkrTextureOpaqueHandle prefilter_opaque =
          vkr_world_resources_resolve_backend_texture(
              &rf->texture_system, prefilter, VKR_TEXTURE_TYPE_CUBE_MAP);
      if (source_opaque && irradiance_opaque && prefilter_opaque &&
          vkr_world_resources_bake_cubemap(
              rf, resources, "shader.ibl.diffuse_convolution",
              resources->ibl_diffuse_bake_pipeline,
              resources->ibl_diffuse_bake_instance_state, source_opaque,
              irradiance_opaque, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, 1u,
              false_v) &&
          vkr_world_resources_bake_cubemap(
              rf, resources, "shader.ibl.specular_prefilter",
              resources->ibl_specular_bake_pipeline,
              resources->ibl_specular_bake_instance_state, source_opaque,
              prefilter_opaque, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE,
              vkr_world_resources_calculate_mip_count(
                  VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE),
              true_v)) {
        resources->ibl_fallback_irradiance_cubemap = irradiance;
        resources->ibl_fallback_prefilter_cubemap = prefilter;
        baked_fallback = true_v;
      } else {
        vkr_world_resources_release_texture(&rf->texture_system, &irradiance);
        vkr_world_resources_release_texture(&rf->texture_system, &prefilter);
      }
    } else {
      vkr_world_resources_release_texture(&rf->texture_system, &irradiance);
      vkr_world_resources_release_texture(&rf->texture_system, &prefilter);
    }
  }

  if (!baked_fallback) {
    resources->ibl_fallback_irradiance_cubemap = fallback_source;
    resources->ibl_fallback_prefilter_cubemap = fallback_source;
    vkr_texture_system_add_ref_by_handle(&rf->texture_system, fallback_source);
    vkr_texture_system_add_ref_by_handle(&rf->texture_system, fallback_source);
  }

  resources->ibl_default_ready = true_v;
  return true_v;
}

void vkr_world_resources_bake_scene_ibl_if_pending(RendererFrontend *rf,
                                                   VkrWorldResources *resources,
                                                   VkrScene *scene) {
  if (!rf || !resources || !scene) {
    return;
  }
  if (!scene->environment.enabled ||
      scene->environment.bake_state != VKR_SCENE_ENV_BAKE_STATE_PENDING) {
    return;
  }

  if (!vkr_world_resources_texture_is_valid(&rf->texture_system,
                                            scene->environment.source_cubemap,
                                            VKR_TEXTURE_TYPE_CUBE_MAP)) {
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
    return;
  }

  if (scene->environment.irradiance_cubemap.id != 0) {
    vkr_texture_system_release_by_handle(&rf->texture_system,
                                         scene->environment.irradiance_cubemap);
  }
  if (scene->environment.prefilter_cubemap.id != 0) {
    vkr_texture_system_release_by_handle(&rf->texture_system,
                                         scene->environment.prefilter_cubemap);
  }
  scene->environment.irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
  scene->environment.prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;

  if (!vkr_world_resources_ensure_ibl_bake_runtime_ready(rf, resources)) {
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
    return;
  }

  char irradiance_name_storage[128];
  char prefilter_name_storage[128];
  int32_t irradiance_written =
      snprintf(irradiance_name_storage, sizeof(irradiance_name_storage),
               "__ibl.scene.%p.irradiance", (void *)scene);
  int32_t prefilter_written =
      snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
               "__ibl.scene.%p.prefilter", (void *)scene);
  if (irradiance_written <= 0 || prefilter_written <= 0) {
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
    return;
  }

  VkrTextureHandle irradiance = VKR_TEXTURE_HANDLE_INVALID;
  VkrTextureHandle prefilter = VKR_TEXTURE_HANDLE_INVALID;
  String8 irradiance_name =
      string8_create_from_cstr((const uint8_t *)irradiance_name_storage,
                               string_length(irradiance_name_storage));
  String8 prefilter_name =
      string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                               string_length(prefilter_name_storage));

  if (!vkr_world_resources_create_writable_cube_texture(
          rf, irradiance_name, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, false_v,
          &irradiance) ||
      !vkr_world_resources_create_writable_cube_texture(
          rf, prefilter_name, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE, true_v,
          &prefilter)) {
    vkr_world_resources_release_texture(&rf->texture_system, &irradiance);
    vkr_world_resources_release_texture(&rf->texture_system, &prefilter);
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
    return;
  }

  VkrTextureOpaqueHandle source_opaque =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, scene->environment.source_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP);
  VkrTextureOpaqueHandle irradiance_opaque =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, irradiance, VKR_TEXTURE_TYPE_CUBE_MAP);
  VkrTextureOpaqueHandle prefilter_opaque =
      vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, prefilter, VKR_TEXTURE_TYPE_CUBE_MAP);
  if (!source_opaque || !irradiance_opaque || !prefilter_opaque ||
      !vkr_world_resources_bake_cubemap(
          rf, resources, "shader.ibl.diffuse_convolution",
          resources->ibl_diffuse_bake_pipeline,
          resources->ibl_diffuse_bake_instance_state, source_opaque,
          irradiance_opaque, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, 1u,
          false_v) ||
      !vkr_world_resources_bake_cubemap(
          rf, resources, "shader.ibl.specular_prefilter",
          resources->ibl_specular_bake_pipeline,
          resources->ibl_specular_bake_instance_state, source_opaque,
          prefilter_opaque, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE,
          vkr_world_resources_calculate_mip_count(
              VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE),
          true_v)) {
    vkr_world_resources_release_texture(&rf->texture_system, &irradiance);
    vkr_world_resources_release_texture(&rf->texture_system, &prefilter);
    scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_FAILED;
    return;
  }

  scene->environment.irradiance_cubemap = irradiance;
  scene->environment.prefilter_cubemap = prefilter;
  scene->environment.bake_state = VKR_SCENE_ENV_BAKE_STATE_READY;
}

void vkr_world_resources_bake_scene_reflection_probes_if_pending(
    RendererFrontend *rf, VkrWorldResources *resources, VkrScene *scene) {
  if (!rf || !resources || !scene || scene->reflection_probe_count == 0) {
    return;
  }

  bool8_t has_pending = false_v;
  uint32_t baked_ready_count = 0;
  uint32_t baked_failed_count = 0;
  for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
    const VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
    if (probe->enabled &&
        probe->bake_state == VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING) {
      has_pending = true_v;
      break;
    }
  }
  if (!has_pending) {
    return;
  }

  if (!vkr_world_resources_ensure_ibl_bake_runtime_ready(rf, resources)) {
    for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
      VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
      if (probe->enabled &&
          probe->bake_state == VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING) {
        probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_FAILED;
      }
    }
    return;
  }

  for (uint32_t i = 0; i < scene->reflection_probe_count; ++i) {
    VkrSceneReflectionProbe *probe = &scene->reflection_probes[i];
    if (!probe->enabled ||
        probe->bake_state != VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_PENDING) {
      continue;
    }

    if (!vkr_world_resources_texture_is_valid(&rf->texture_system,
                                              probe->source_cubemap,
                                              VKR_TEXTURE_TYPE_CUBE_MAP)) {
      probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_FAILED;
      baked_failed_count++;
      continue;
    }

    if (probe->irradiance_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(&rf->texture_system,
                                           probe->irradiance_cubemap);
      probe->irradiance_cubemap = VKR_TEXTURE_HANDLE_INVALID;
    }
    if (probe->prefilter_cubemap.id != 0) {
      vkr_texture_system_release_by_handle(&rf->texture_system,
                                           probe->prefilter_cubemap);
      probe->prefilter_cubemap = VKR_TEXTURE_HANDLE_INVALID;
    }

    char irradiance_name_storage[160];
    char prefilter_name_storage[160];
    int32_t irradiance_written =
        snprintf(irradiance_name_storage, sizeof(irradiance_name_storage),
                 "__ibl.scene.%p.probe.%u.irradiance", (void *)scene, i);
    int32_t prefilter_written =
        snprintf(prefilter_name_storage, sizeof(prefilter_name_storage),
                 "__ibl.scene.%p.probe.%u.prefilter", (void *)scene, i);
    if (irradiance_written <= 0 || prefilter_written <= 0) {
      probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_FAILED;
      baked_failed_count++;
      continue;
    }

    VkrTextureHandle irradiance = VKR_TEXTURE_HANDLE_INVALID;
    VkrTextureHandle prefilter = VKR_TEXTURE_HANDLE_INVALID;
    String8 irradiance_name =
        string8_create_from_cstr((const uint8_t *)irradiance_name_storage,
                                 string_length(irradiance_name_storage));
    String8 prefilter_name =
        string8_create_from_cstr((const uint8_t *)prefilter_name_storage,
                                 string_length(prefilter_name_storage));
    if (!vkr_world_resources_create_writable_cube_texture(
            rf, irradiance_name, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE,
            false_v, &irradiance) ||
        !vkr_world_resources_create_writable_cube_texture(
            rf, prefilter_name, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE, true_v,
            &prefilter)) {
      vkr_world_resources_release_texture(&rf->texture_system, &irradiance);
      vkr_world_resources_release_texture(&rf->texture_system, &prefilter);
      probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_FAILED;
      baked_failed_count++;
      continue;
    }

    VkrTextureOpaqueHandle source_opaque =
        vkr_world_resources_resolve_backend_texture(&rf->texture_system,
                                                    probe->source_cubemap,
                                                    VKR_TEXTURE_TYPE_CUBE_MAP);
    VkrTextureOpaqueHandle irradiance_opaque =
        vkr_world_resources_resolve_backend_texture(
            &rf->texture_system, irradiance, VKR_TEXTURE_TYPE_CUBE_MAP);
    VkrTextureOpaqueHandle prefilter_opaque =
        vkr_world_resources_resolve_backend_texture(
            &rf->texture_system, prefilter, VKR_TEXTURE_TYPE_CUBE_MAP);
    bool8_t baked =
        source_opaque && irradiance_opaque && prefilter_opaque &&
        vkr_world_resources_bake_cubemap(
            rf, resources, "shader.ibl.diffuse_convolution",
            resources->ibl_diffuse_bake_pipeline,
            resources->ibl_diffuse_bake_instance_state, source_opaque,
            irradiance_opaque, VKR_WORLD_RESOURCES_IBL_IRRADIANCE_SIZE, 1u,
            false_v) &&
        vkr_world_resources_bake_cubemap(
            rf, resources, "shader.ibl.specular_prefilter",
            resources->ibl_specular_bake_pipeline,
            resources->ibl_specular_bake_instance_state, source_opaque,
            prefilter_opaque, VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE,
            vkr_world_resources_calculate_mip_count(
                VKR_WORLD_RESOURCES_IBL_PREFILTER_SIZE),
            true_v);

    if (!baked) {
      vkr_world_resources_release_texture(&rf->texture_system, &irradiance);
      vkr_world_resources_release_texture(&rf->texture_system, &prefilter);
      probe->bake_state = VKR_SCENE_REFLECTION_PROBE_BAKE_STATE_FAILED;
      baked_failed_count++;
      continue;
    }

    probe->irradiance_cubemap = irradiance;
    probe->prefilter_cubemap = prefilter;
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

vkr_internal float32_t vkr_world_resources_probe_influence(
    const VkrSceneReflectionProbe *probe, Vec3 world_position) {
  if (!probe) {
    return 0.0f;
  }

  float32_t dx = vkr_abs_f32(world_position.x - probe->center.x);
  float32_t dy = vkr_abs_f32(world_position.y - probe->center.y);
  float32_t dz = vkr_abs_f32(world_position.z - probe->center.z);
  float32_t outside_x = vkr_max_f32(0.0f, dx - probe->extents.x);
  float32_t outside_y = vkr_max_f32(0.0f, dy - probe->extents.y);
  float32_t outside_z = vkr_max_f32(0.0f, dz - probe->extents.z);
  float32_t outside_distance = vkr_sqrt_f32(
      outside_x * outside_x + outside_y * outside_y + outside_z * outside_z);

  if (outside_distance <= 1e-6f) {
    return 1.0f;
  }
  if (probe->blend_distance <= 0.0f) {
    return 0.0f;
  }

  return vkr_max_f32(0.0f, 1.0f - outside_distance / probe->blend_distance);
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
    Vec3 world_position, VkrWorldIblProbeSlot out_slots[2]) {
  if (!out_slots) {
    return;
  }

  MemZero(out_slots, sizeof(VkrWorldIblProbeSlot) * 2u);
  if (!rf || !resources) {
    out_slots[0].weight = 1.0f;
    return;
  }

  if (!resources->ibl_default_ready) {
    if (!vkr_world_resources_ensure_default_ibl_ready(rf, resources)) {
      out_slots[0].weight = 1.0f;
      return;
    }
  }

  VkrWorldIblProbeSlot fallback =
      vkr_world_resources_fallback_probe_slot(rf, resources);

  uint32_t best_probe_index[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
  float32_t best_probe_influence[2] = {0.0f, 0.0f};
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

      float32_t influence =
          vkr_world_resources_probe_influence(probe, world_position);
      if (influence <= 0.0f) {
        continue;
      }

      if (influence > best_probe_influence[0]) {
        best_probe_influence[1] = best_probe_influence[0];
        best_probe_index[1] = best_probe_index[0];
        best_probe_influence[0] = influence;
        best_probe_index[0] = i;
      } else if (influence > best_probe_influence[1]) {
        best_probe_influence[1] = influence;
        best_probe_index[1] = i;
      }
    }
  }

  const bool8_t has_first_local = best_probe_index[0] != 0xFFFFFFFFu;
  const bool8_t has_second_local = best_probe_index[1] != 0xFFFFFFFFu;

  if (!has_first_local) {
    out_slots[0] = fallback;
    out_slots[1] = fallback;
    out_slots[0].weight = 1.0f;
    out_slots[1].weight = 0.0f;
    return;
  }

  const VkrSceneReflectionProbe *probe0 =
      &scene->reflection_probes[best_probe_index[0]];
  VkrWorldIblProbeSlot slot0 = {
      .irradiance_map = vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, probe0->irradiance_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP),
      .prefilter_map = vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, probe0->prefilter_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP),
      .center = probe0->center,
      .extents = probe0->extents,
      .blend_distance = probe0->blend_distance,
      .weight = 1.0f,
      .intensity = probe0->intensity,
      .diffuse_intensity = probe0->diffuse_intensity,
      .specular_intensity = probe0->specular_intensity,
      .box_projection_enabled = true_v,
  };

  if (!has_second_local) {
    float32_t local_weight = vkr_clamp_f32(best_probe_influence[0], 0.0f, 1.0f);
    out_slots[0] = slot0;
    out_slots[1] = fallback;
    out_slots[0].weight = local_weight;
    out_slots[1].weight = 1.0f - local_weight;
    return;
  }

  const VkrSceneReflectionProbe *probe1 =
      &scene->reflection_probes[best_probe_index[1]];
  VkrWorldIblProbeSlot slot1 = {
      .irradiance_map = vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, probe1->irradiance_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP),
      .prefilter_map = vkr_world_resources_resolve_backend_texture(
          &rf->texture_system, probe1->prefilter_cubemap,
          VKR_TEXTURE_TYPE_CUBE_MAP),
      .center = probe1->center,
      .extents = probe1->extents,
      .blend_distance = probe1->blend_distance,
      .weight = 1.0f,
      .intensity = probe1->intensity,
      .diffuse_intensity = probe1->diffuse_intensity,
      .specular_intensity = probe1->specular_intensity,
      .box_projection_enabled = true_v,
  };

  float32_t weight_sum = best_probe_influence[0] + best_probe_influence[1];
  if (weight_sum <= 1e-6f) {
    out_slots[0] = fallback;
    out_slots[1] = fallback;
    out_slots[0].weight = 1.0f;
    out_slots[1].weight = 0.0f;
    return;
  }

  out_slots[0] = slot0;
  out_slots[1] = slot1;
  out_slots[0].weight = best_probe_influence[0] / weight_sum;
  out_slots[1].weight = best_probe_influence[1] / weight_sum;
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

  vkr_world_resources_release_texture(
      &rf->texture_system, &resources->ibl_fallback_prefilter_cubemap);
  vkr_world_resources_release_texture(
      &rf->texture_system, &resources->ibl_fallback_irradiance_cubemap);
  vkr_world_resources_release_texture(&rf->texture_system,
                                      &resources->ibl_fallback_source_cubemap);
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
