/**
 * @file vkr_picking_system.c
 * @brief Implementation of the picking system for pixel-perfect object
 * selection.
 */

#include <stdlib.h>

#include "core/logger.h"
#include "defines.h"
#include "math/vec.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/views/vkr_view_ui.h"
#include "renderer/systems/views/vkr_view_world.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "vkr_picking_ids.h"
#include "vkr_picking_system.h"

/**
 * @brief Alpha test threshold for transparency-aware picking.
 *
 * Picking resolves to a single object ID per pixel. For cutout textures, using
 * alpha_cutoff prevents "invisible" texels from occluding geometry behind
 * them.
 */

// ============================================================================
// Internal helpers
// ============================================================================

typedef struct VkrPickingTransparentSubmeshEntry {
  uint32_t mesh_index;
  uint32_t submesh_index;
  float32_t distance;
} VkrPickingTransparentSubmeshEntry;

vkr_internal int vkr_picking_transparent_submesh_compare(const void *a,
                                                         const void *b) {
  const VkrPickingTransparentSubmeshEntry *entry_a =
      (const VkrPickingTransparentSubmeshEntry *)a;
  const VkrPickingTransparentSubmeshEntry *entry_b =
      (const VkrPickingTransparentSubmeshEntry *)b;

  if (entry_a->distance > entry_b->distance)
    return -1;
  if (entry_a->distance < entry_b->distance)
    return 1;
  return 0;
}

/**
 * @brief Create picking attachments (color texture + depth buffer).
 */
vkr_internal bool8_t picking_create_attachments(RendererFrontend *rf,
                                                VkrPickingContext *ctx,
                                                uint32_t width,
                                                uint32_t height) {
  assert_log(rf != NULL, "Renderer is NULL");
  assert_log(ctx != NULL, "Picking context is NULL");

  if (width == 0 || height == 0) {
    log_error("Invalid picking dimensions: %ux%u", width, height);
    return false_v;
  }

  // Create R32_UINT color attachment for object IDs
  VkrRenderTargetTextureDesc color_desc = {
      .width = width,
      .height = height,
      .format = VKR_TEXTURE_FORMAT_R32_UINT,
      .usage = vkr_texture_usage_flags_from_bits(
          VKR_TEXTURE_USAGE_COLOR_ATTACHMENT | VKR_TEXTURE_USAGE_TRANSFER_SRC),
  };

  VkrRendererError color_err = VKR_RENDERER_ERROR_NONE;
  ctx->picking_texture =
      vkr_renderer_create_render_target_texture(rf, &color_desc, &color_err);
  if (!ctx->picking_texture || color_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(color_err);
    log_error("Failed to create picking color attachment: %s",
              string8_cstr(&err_str));
    return false_v;
  }

  VkrRendererError depth_err = VKR_RENDERER_ERROR_NONE;
  ctx->picking_depth =
      vkr_renderer_create_depth_attachment(rf, width, height, &depth_err);
  if (!ctx->picking_depth || depth_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(depth_err);
    log_error("Failed to create picking depth attachment: %s",
              string8_cstr(&err_str));
    vkr_renderer_destroy_texture(rf, ctx->picking_texture);
    ctx->picking_texture = NULL;
    return false_v;
  }

  ctx->width = width;
  ctx->height = height;
  return true_v;
}

/**
 * @brief Destroy picking attachments.
 */
vkr_internal void picking_destroy_attachments(RendererFrontend *rf,
                                              VkrPickingContext *ctx) {
  assert_log(rf != NULL, "Renderer is NULL");
  assert_log(ctx != NULL, "Picking context is NULL");

  if (ctx->picking_target) {
    vkr_renderer_render_target_destroy(rf, ctx->picking_target, false_v);
    ctx->picking_target = NULL;
  }

  if (ctx->picking_texture) {
    vkr_renderer_destroy_texture(rf, ctx->picking_texture);
    ctx->picking_texture = NULL;
  }

  if (ctx->picking_depth) {
    vkr_renderer_destroy_texture(rf, ctx->picking_depth);
    ctx->picking_depth = NULL;
  }
}

vkr_internal void picking_release_pipeline(RendererFrontend *rf,
                                           VkrPipelineHandle *pipeline) {
  if (!rf || !pipeline || pipeline->id == 0) {
    return;
  }

  vkr_pipeline_registry_release(&rf->pipeline_registry, *pipeline);
  *pipeline = VKR_PIPELINE_HANDLE_INVALID;
}

vkr_internal void
picking_release_instance_state(RendererFrontend *rf, VkrPipelineHandle pipeline,
                               VkrRendererInstanceStateHandle *instance_state) {
  if (!rf || !instance_state || pipeline.id == 0 ||
      instance_state->id == VKR_INVALID_ID) {
    return;
  }

  VkrRendererError err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_release_instance_state(
          &rf->pipeline_registry, pipeline, *instance_state, &err)) {
    String8 err_str = vkr_renderer_get_error_string(err);
    log_warn("Failed to release picking instance state: %s",
             string8_cstr(&err_str));
  }

  instance_state->id = VKR_INVALID_ID;
}

/**
 * @brief Create picking render target using existing pass.
 */
vkr_internal bool8_t picking_create_render_target(RendererFrontend *rf,
                                                  VkrPickingContext *ctx) {
  assert_log(rf != NULL, "Renderer is NULL");
  assert_log(ctx != NULL, "Picking context is NULL");

  if (!ctx->picking_pass) {
    log_error("Picking render pass not available");
    return false_v;
  }

  VkrTextureOpaqueHandle attachments[2] = {ctx->picking_texture,
                                           ctx->picking_depth};
  VkrRenderTargetDesc rt_desc = {
      .sync_to_window_size = false_v,
      .attachment_count = 2,
      .attachments = attachments,
      .width = ctx->width,
      .height = ctx->height,
  };

  ctx->picking_target =
      vkr_renderer_render_target_create(rf, &rt_desc, ctx->picking_pass);
  if (!ctx->picking_target) {
    log_error("Failed to create picking render target");
    return false_v;
  }

  return true_v;
}

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Prepare and apply per-submesh material state for picking.
 *
 * Resolves diffuse texture and alpha cutoff, applies local material state,
 * sets shader uniforms/samplers and applies the shader instance.
 *
 * Returns true when the shader instance was applied successfully and the
 * caller can proceed with geometry rendering.
 */
vkr_internal bool8_t picking_render_submesh(
    RendererFrontend *rf, VkrInstanceBufferPool *instance_pool, VkrMesh *mesh,
    VkrSubMesh *submesh, VkrTextureOpaqueHandle fallback_texture,
    bool8_t can_alpha_test, uint32_t *out_first_instance) {
  if (!rf || !instance_pool || !mesh || !submesh || !out_first_instance) {
    return false_v;
  }

  if (!instance_pool->initialized) {
    return false_v;
  }

  Mat4 model = mesh->model;
  uint32_t object_id =
      mesh->render_id
          ? vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE, mesh->render_id)
          : 0;

  VkrTextureOpaqueHandle diffuse_texture_handle = fallback_texture;
  float32_t alpha_cutoff = 0.0f;

  if (submesh->material.id != 0) {
    VkrMaterial *material = vkr_material_system_get_by_handle(
        &rf->material_system, submesh->material);
    if (material) {
      VkrMaterialTexture *diffuse_tex =
          &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
      if (diffuse_tex->enabled && diffuse_tex->handle.id != 0) {
        VkrTexture *texture = vkr_texture_system_get_by_handle(
            &rf->texture_system, diffuse_tex->handle);
        // Only use 2D textures - cubemaps/arrays are incompatible with the
        // picking shader's sampler2D descriptor.
        if (texture && texture->handle &&
            texture->description.type == VKR_TEXTURE_TYPE_2D) {
          diffuse_texture_handle = texture->handle;
          if (can_alpha_test && material->alpha_cutoff > 0.0f) {
            alpha_cutoff = material->alpha_cutoff;
          }
        }
      }
    }
  }

  VkrInstanceDataGPU *instance_ptr = NULL;
  uint32_t base_instance = 0;
  if (!vkr_instance_buffer_alloc(instance_pool, 1, &base_instance,
                                 &instance_ptr)) {
    return false_v;
  }

  *instance_ptr = (VkrInstanceDataGPU){
      .model = model,
      .object_id = object_id,
      .material_index = 0,
      .flags = 0,
      ._padding = 0,
  };
  vkr_instance_buffer_flush_range(instance_pool, base_instance, 1);

  vkr_shader_system_uniform_set(&rf->shader_system, "alpha_cutoff",
                                &alpha_cutoff);

  if (diffuse_texture_handle) {
    vkr_shader_system_sampler_set(&rf->shader_system, "diffuse_texture",
                                  diffuse_texture_handle);
  }

  if (!vkr_shader_system_apply_instance(&rf->shader_system)) {
    return false_v;
  }
  *out_first_instance = base_instance;

  return true_v;
}

bool8_t vkr_picking_init(struct s_RendererFrontend *renderer,
                         VkrPickingContext *ctx, uint32_t width,
                         uint32_t height) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(ctx != NULL, "Picking context is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  MemZero(ctx, sizeof(VkrPickingContext));
  ctx->mesh_instance_state.id = VKR_INVALID_ID;
  ctx->mesh_overlay_instance_state.id = VKR_INVALID_ID;
  ctx->mesh_transparent_instance_state.id = VKR_INVALID_ID;

  if (width == 0 || height == 0) {
    log_error("Invalid picking dimensions: %ux%u", width, height);
    return false_v;
  }

  ctx->picking_pass = vkr_renderer_renderpass_get(
      rf, string8_lit("Renderpass.Builtin.Picking"));
  if (!ctx->picking_pass) {
    VkrRenderPassConfig pass_cfg = {
        .name = string8_lit("Renderpass.Builtin.Picking"),
        .clear_color = {0.0f, 0.0f, 0.0f, 0.0f},
        .clear_flags = VKR_RENDERPASS_CLEAR_COLOR | VKR_RENDERPASS_CLEAR_DEPTH,
        .domain = VKR_PIPELINE_DOMAIN_PICKING,
    };
    ctx->picking_pass = vkr_renderer_renderpass_create(rf, &pass_cfg);
    if (!ctx->picking_pass) {
      log_error("Failed to create picking render pass");
      return false_v;
    }
  }

  if (!picking_create_attachments(rf, ctx, width, height)) {
    log_error("Failed to create picking attachments");
    return false_v;
  }

  if (!picking_create_render_target(rf, ctx)) {
    log_error("Failed to create picking render target");
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  VkrResourceHandleInfo cfg_info = {0};
  VkrRendererError shadercfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/picking.shadercfg"),
          &rf->scratch_allocator, &cfg_info, &shadercfg_err)) {
    String8 err_str = vkr_renderer_get_error_string(shadercfg_err);
    log_error("Failed to load picking shader config: %s",
              string8_cstr(&err_str));
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  if (!cfg_info.as.custom) {
    log_error("Shader config returned null custom data");
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  ctx->shader_config = *(VkrShaderConfig *)cfg_info.as.custom;

  if (!vkr_shader_system_create(&rf->shader_system, &ctx->shader_config)) {
    log_error("Failed to create picking shader in shader system");
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  VkrRendererError pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &ctx->shader_config,
          VKR_PIPELINE_DOMAIN_PICKING, string8_lit("picking"),
          &ctx->picking_pipeline, &pipeline_err)) {
    String8 err_str = vkr_renderer_get_error_string(pipeline_err);
    log_error("Failed to create picking pipeline: %s", string8_cstr(&err_str));
    vkr_shader_system_delete(&rf->shader_system, "shader.picking");
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  if (ctx->shader_config.name.str && ctx->shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, ctx->picking_pipeline, ctx->shader_config.name,
        &alias_err);
  }

  if (ctx->shader_config.instance_texture_count > 0) {
    VkrRendererError instance_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, ctx->picking_pipeline,
            &ctx->mesh_instance_state, &instance_err)) {
      String8 err_str = vkr_renderer_get_error_string(instance_err);
      log_error("Failed to acquire picking instance state: %s",
                string8_cstr(&err_str));
      picking_release_pipeline(rf, &ctx->picking_pipeline);
      vkr_shader_system_delete(&rf->shader_system, "shader.picking");
      picking_destroy_attachments(rf, ctx);
      return false_v;
    }
  }

  // Create a transparent picking pipeline variant (depth-tested, depth-write
  // off) to match the visible render path for transparent submeshes and avoid
  // falsely occluding world text behind them.
  {
    VkrShaderConfig transparent_cfg = ctx->shader_config;
    transparent_cfg.name = (String8){0};

    VkrRendererError transparent_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_create_from_shader_config(
            &rf->pipeline_registry, &transparent_cfg,
            VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT,
            string8_lit("picking_transparent"),
            &ctx->picking_transparent_pipeline, &transparent_err)) {
      String8 err_str = vkr_renderer_get_error_string(transparent_err);
      log_warn("Failed to create transparent picking pipeline: %s",
               string8_cstr(&err_str));
      ctx->picking_transparent_pipeline = VKR_PIPELINE_HANDLE_INVALID;
    } else if (ctx->shader_config.instance_texture_count > 0) {
      VkrRendererError transparent_instance_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_pipeline_registry_acquire_instance_state(
              &rf->pipeline_registry, ctx->picking_transparent_pipeline,
              &ctx->mesh_transparent_instance_state,
              &transparent_instance_err)) {
        String8 err_str =
            vkr_renderer_get_error_string(transparent_instance_err);
        log_warn("Failed to acquire transparent picking instance state: %s",
                 string8_cstr(&err_str));
        picking_release_pipeline(rf, &ctx->picking_transparent_pipeline);
        ctx->mesh_transparent_instance_state.id = VKR_INVALID_ID;
      }
    }
  }

  // Create an overlay picking pipeline (no depth test/write) for gizmo handles.
  {
    VkrShaderConfig overlay_cfg = ctx->shader_config;
    overlay_cfg.name = (String8){0};

    VkrRendererError overlay_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_create_from_shader_config(
            &rf->pipeline_registry, &overlay_cfg,
            VKR_PIPELINE_DOMAIN_PICKING_OVERLAY, string8_lit("picking_overlay"),
            &ctx->picking_overlay_pipeline, &overlay_err)) {
      String8 err_str = vkr_renderer_get_error_string(overlay_err);
      log_warn("Failed to create overlay picking pipeline: %s",
               string8_cstr(&err_str));
      ctx->picking_overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
    } else if (ctx->shader_config.instance_texture_count > 0) {
      VkrRendererError overlay_instance_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_pipeline_registry_acquire_instance_state(
              &rf->pipeline_registry, ctx->picking_overlay_pipeline,
              &ctx->mesh_overlay_instance_state, &overlay_instance_err)) {
        String8 err_str = vkr_renderer_get_error_string(overlay_instance_err);
        log_warn("Failed to acquire overlay picking instance state: %s",
                 string8_cstr(&err_str));
        picking_release_pipeline(rf, &ctx->picking_overlay_pipeline);
        ctx->mesh_overlay_instance_state.id = VKR_INVALID_ID;
      }
    }
  }

  VkrResourceHandleInfo text_cfg_info = {0};
  VkrRendererError text_cfg_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_resource_system_load_custom(
          string8_lit("shadercfg"),
          string8_lit("assets/shaders/picking_text.shadercfg"),
          &rf->scratch_allocator, &text_cfg_info, &text_cfg_err)) {
    String8 err_str = vkr_renderer_get_error_string(text_cfg_err);
    log_error("Failed to load picking text shader config: %s",
              string8_cstr(&err_str));
    picking_release_instance_state(rf, ctx->picking_transparent_pipeline,
                                   &ctx->mesh_transparent_instance_state);
    picking_release_pipeline(rf, &ctx->picking_transparent_pipeline);
    picking_release_instance_state(rf, ctx->picking_overlay_pipeline,
                                   &ctx->mesh_overlay_instance_state);
    picking_release_pipeline(rf, &ctx->picking_overlay_pipeline);
    picking_release_instance_state(rf, ctx->picking_pipeline,
                                   &ctx->mesh_instance_state);
    picking_release_pipeline(rf, &ctx->picking_pipeline);
    vkr_shader_system_delete(&rf->shader_system, "shader.picking");
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  if (!text_cfg_info.as.custom) {
    log_error("Picking text shader config returned null custom data");
    picking_release_instance_state(rf, ctx->picking_transparent_pipeline,
                                   &ctx->mesh_transparent_instance_state);
    picking_release_pipeline(rf, &ctx->picking_transparent_pipeline);
    picking_release_instance_state(rf, ctx->picking_overlay_pipeline,
                                   &ctx->mesh_overlay_instance_state);
    picking_release_pipeline(rf, &ctx->picking_overlay_pipeline);
    picking_release_instance_state(rf, ctx->picking_pipeline,
                                   &ctx->mesh_instance_state);
    picking_release_pipeline(rf, &ctx->picking_pipeline);
    vkr_shader_system_delete(&rf->shader_system, "shader.picking");
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  ctx->text_shader_config = *(VkrShaderConfig *)text_cfg_info.as.custom;

  if (!vkr_shader_system_create(&rf->shader_system, &ctx->text_shader_config)) {
    log_error("Failed to create picking text shader in shader system");
    picking_release_instance_state(rf, ctx->picking_transparent_pipeline,
                                   &ctx->mesh_transparent_instance_state);
    picking_release_pipeline(rf, &ctx->picking_transparent_pipeline);
    picking_release_instance_state(rf, ctx->picking_overlay_pipeline,
                                   &ctx->mesh_overlay_instance_state);
    picking_release_pipeline(rf, &ctx->picking_overlay_pipeline);
    picking_release_instance_state(rf, ctx->picking_pipeline,
                                   &ctx->mesh_instance_state);
    picking_release_pipeline(rf, &ctx->picking_pipeline);
    vkr_shader_system_delete(&rf->shader_system, "shader.picking");
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  VkrShaderConfig text_shader_config = ctx->text_shader_config;
  text_shader_config.cull_mode = VKR_CULL_MODE_NONE;

  VkrRendererError text_pipeline_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_create_from_shader_config(
          &rf->pipeline_registry, &text_shader_config,
          // Text picking should behave like an overlay: draw last and always
          // win the ID buffer regardless of depth.
          VKR_PIPELINE_DOMAIN_POST, string8_lit("picking_text"),
          &ctx->picking_text_pipeline, &text_pipeline_err)) {
    String8 err_str = vkr_renderer_get_error_string(text_pipeline_err);
    log_error("Failed to create picking text pipeline: %s",
              string8_cstr(&err_str));
    vkr_shader_system_delete(&rf->shader_system, "shader.picking_text");
    picking_release_instance_state(rf, ctx->picking_transparent_pipeline,
                                   &ctx->mesh_transparent_instance_state);
    picking_release_pipeline(rf, &ctx->picking_transparent_pipeline);
    picking_release_instance_state(rf, ctx->picking_overlay_pipeline,
                                   &ctx->mesh_overlay_instance_state);
    picking_release_pipeline(rf, &ctx->picking_overlay_pipeline);
    picking_release_instance_state(rf, ctx->picking_pipeline,
                                   &ctx->mesh_instance_state);
    picking_release_pipeline(rf, &ctx->picking_pipeline);
    vkr_shader_system_delete(&rf->shader_system, "shader.picking");
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  // Create a WORLD text picking pipeline variant (depth-tested, depth-write
  // off) so world text picking respects the scene depth buffer.
  {
    VkrShaderConfig world_text_cfg = text_shader_config;
    world_text_cfg.name = (String8){0};
    VkrRendererError world_text_pipeline_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_create_from_shader_config(
            &rf->pipeline_registry, &world_text_cfg,
            VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT,
            string8_lit("picking_world_text"),
            &ctx->picking_world_text_pipeline, &world_text_pipeline_err)) {
      String8 err_str = vkr_renderer_get_error_string(world_text_pipeline_err);
      log_error("Failed to create world picking text pipeline: %s",
                string8_cstr(&err_str));
      picking_release_pipeline(rf, &ctx->picking_text_pipeline);
      vkr_shader_system_delete(&rf->shader_system, "shader.picking_text");
      picking_release_instance_state(rf, ctx->picking_transparent_pipeline,
                                     &ctx->mesh_transparent_instance_state);
      picking_release_pipeline(rf, &ctx->picking_transparent_pipeline);
      picking_release_instance_state(rf, ctx->picking_overlay_pipeline,
                                     &ctx->mesh_overlay_instance_state);
      picking_release_pipeline(rf, &ctx->picking_overlay_pipeline);
      picking_release_instance_state(rf, ctx->picking_pipeline,
                                     &ctx->mesh_instance_state);
      picking_release_pipeline(rf, &ctx->picking_pipeline);
      vkr_shader_system_delete(&rf->shader_system, "shader.picking");
      picking_destroy_attachments(rf, ctx);
      return false_v;
    }
  }

  if (ctx->text_shader_config.name.str &&
      ctx->text_shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, ctx->picking_text_pipeline,
        ctx->text_shader_config.name, &alias_err);
  }

  // Create unit cube for light gizmo picking
  VkrRendererError cube_err = VKR_RENDERER_ERROR_NONE;
  ctx->light_gizmo_cube = vkr_geometry_system_create_cube(
      &rf->geometry_system, 1.0f, 1.0f, 1.0f, "light_gizmo_cube", &cube_err);
  if (!ctx->light_gizmo_cube.id) {
    log_warn("Failed to create light gizmo cube - light picking disabled");
  }

  ctx->state = VKR_PICKING_STATE_IDLE;
  ctx->initialized = true_v;

  log_debug("Picking system initialized: %ux%u", width, height);
  return true_v;
}

void vkr_picking_resize(struct s_RendererFrontend *renderer,
                        VkrPickingContext *ctx, uint32_t new_width,
                        uint32_t new_height) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(ctx != NULL, "Picking context is NULL");

  if (!ctx->initialized) {
    log_warn("Picking system not initialized, cannot resize");
    return;
  }

  if (new_width == 0 || new_height == 0) {
    log_warn("Invalid resize dimensions: %ux%u", new_width, new_height);
    return;
  }

  if (ctx->width == new_width && ctx->height == new_height) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;

  if (vkr_renderer_wait_idle(rf) != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to wait for renderer to be idle");
    return;
  }

  picking_destroy_attachments(rf, ctx);

  if (!picking_create_attachments(rf, ctx, new_width, new_height)) {
    log_error("Failed to recreate picking attachments on resize");
    ctx->initialized = false_v;
    return;
  }

  if (!picking_create_render_target(rf, ctx)) {
    log_error("Failed to recreate picking render target on resize");
    picking_destroy_attachments(rf, ctx);
    ctx->initialized = false_v;
    return;
  }
}

void vkr_picking_request(VkrPickingContext *ctx, uint32_t target_x,
                         uint32_t target_y) {
  assert_log(ctx != NULL, "Picking context is NULL");

  if (!ctx->initialized) {
    log_warn("Picking system not initialized");
    return;
  }

  if (ctx->state != VKR_PICKING_STATE_IDLE) {
    return;
  }

  if (target_x >= ctx->width || target_y >= ctx->height) {
    log_warn("Pick coordinates out of bounds: (%u, %u) vs (%u, %u)", target_x,
             target_y, ctx->width, ctx->height);
    return;
  }

  ctx->requested_x = target_x;
  ctx->requested_y = target_y;
  ctx->state = VKR_PICKING_STATE_RENDER_PENDING;
  ctx->result_object_id = 0;
}

void vkr_picking_render(struct s_RendererFrontend *renderer,
                        VkrPickingContext *ctx,
                        struct VkrMeshManager *mesh_manager) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(ctx != NULL, "Picking context is NULL");
  assert_log(mesh_manager != NULL, "Mesh manager is NULL");

  if (!ctx->initialized) {
    return;
  }

  // Only render if a pick is pending
  if (ctx->state != VKR_PICKING_STATE_RENDER_PENDING) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrInstanceBufferPool *instance_pool = &rf->instance_buffer_pool;
  if (!instance_pool->initialized) {
    log_error("Picking render skipped: instance buffer pool not initialized");
    ctx->state = VKR_PICKING_STATE_IDLE;
    return;
  }

  VkrRendererError begin_err = vkr_renderer_begin_render_pass(
      rf, ctx->picking_pass, ctx->picking_target);
  if (begin_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(begin_err);
    log_error("Failed to begin picking render pass: %s",
              string8_cstr(&err_str));
    ctx->state = VKR_PICKING_STATE_IDLE;
    return;
  }

  if (!vkr_shader_system_use(&rf->shader_system, "shader.picking")) {
    log_error("Failed to use picking shader");
    vkr_renderer_end_render_pass(rf);
    ctx->state = VKR_PICKING_STATE_IDLE;
    return;
  }

  VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
  vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry,
                                      ctx->picking_pipeline, &bind_err);
  if (bind_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(bind_err);
    log_error("Failed to bind picking pipeline: %s", string8_cstr(&err_str));
    vkr_renderer_end_render_pass(rf);
    ctx->state = VKR_PICKING_STATE_IDLE;
    return;
  }

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_PICKING);

  // Re-acquire instance states if they were invalidated (e.g., after scene
  // unload). This ensures descriptor sets reference valid textures.
  if (ctx->shader_config.instance_texture_count > 0 &&
      ctx->mesh_instance_state.id == VKR_INVALID_ID) {
    VkrRendererError instance_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, ctx->picking_pipeline,
            &ctx->mesh_instance_state, &instance_err)) {
      log_warn("Failed to re-acquire picking instance state");
    }
  }

  if (ctx->picking_transparent_pipeline.id != 0 &&
      ctx->shader_config.instance_texture_count > 0 &&
      ctx->mesh_transparent_instance_state.id == VKR_INVALID_ID) {
    VkrRendererError instance_err = VKR_RENDERER_ERROR_NONE;
    if (!vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, ctx->picking_transparent_pipeline,
            &ctx->mesh_transparent_instance_state, &instance_err)) {
      log_warn("Failed to re-acquire transparent picking instance state");
    }
  }

  const bool8_t can_alpha_test =
      (ctx->mesh_instance_state.id != VKR_INVALID_ID) ? true_v : false_v;
  if (can_alpha_test) {
    vkr_shader_system_bind_instance(&rf->shader_system,
                                    ctx->mesh_instance_state.id);
  }

  VkrTextureOpaqueHandle fallback_texture = NULL;
  VkrTexture *default_texture =
      vkr_texture_system_get_default(&rf->texture_system);
  if (default_texture) {
    fallback_texture = default_texture->handle;
  }

  uint32_t mesh_capacity = vkr_mesh_manager_capacity(mesh_manager);
  Vec3 camera_pos = rf->globals.view_position;

  const bool8_t has_transparent_pipeline =
      (ctx->picking_transparent_pipeline.id != 0 &&
       ctx->mesh_transparent_instance_state.id != VKR_INVALID_ID)
          ? true_v
          : false_v;

  VkrPickingTransparentSubmeshEntry *transparent_entries = NULL;
  uint32_t transparent_count = 0;
  uint32_t max_transparent_entries = 0;

  VkrAllocatorScope temp_scope = {0};
  if (has_transparent_pipeline) {
    VkrAllocator *temp_alloc = &rf->allocator;
    temp_scope = vkr_allocator_begin_scope(temp_alloc);
    if (vkr_allocator_scope_is_valid(&temp_scope)) {
      for (uint32_t mesh_index = 0; mesh_index < mesh_capacity; mesh_index++) {
        VkrMesh *mesh = vkr_mesh_manager_get(mesh_manager, mesh_index);
        if (!mesh || !mesh->visible) {
          continue;
        }
        max_transparent_entries += vkr_mesh_manager_submesh_count(mesh);
      }

      if (max_transparent_entries > 0) {
        transparent_entries = vkr_allocator_alloc(
            temp_alloc,
            sizeof(VkrPickingTransparentSubmeshEntry) * max_transparent_entries,
            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      }
    }
  }

  for (uint32_t mesh_index = 0; mesh_index < mesh_capacity; mesh_index++) {
    VkrMesh *mesh = vkr_mesh_manager_get(mesh_manager, mesh_index);
    if (!mesh) {
      continue;
    }
    if (!mesh->visible) {
      continue;
    }

    uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    if (submesh_count == 0) {
      continue;
    }

    Mat4 model = mesh->model;

    for (uint32_t submesh_index = 0; submesh_index < submesh_count;
         submesh_index++) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(mesh_manager, mesh_index, submesh_index);
      if (!submesh) {
        continue;
      }

      bool8_t uses_cutout = false_v;

      if (submesh->material.id != 0) {
        VkrMaterial *material = vkr_material_system_get_by_handle(
            &rf->material_system, submesh->material);
        if (material) {
          VkrMaterialTexture *diffuse_tex =
              &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
          if (material->alpha_cutoff > 0.0f && diffuse_tex->enabled &&
              diffuse_tex->handle.id != 0) {
            uses_cutout = true_v;
          }
        }
      }

      if (has_transparent_pipeline && uses_cutout && transparent_entries &&
          transparent_count < max_transparent_entries) {
        Vec3 mesh_pos = vec3_new(model.elements[12], model.elements[13],
                                 model.elements[14]);
        float32_t distance = vec3_distance(mesh_pos, camera_pos);
        transparent_entries[transparent_count++] =
            (VkrPickingTransparentSubmeshEntry){
                .mesh_index = mesh_index,
                .submesh_index = submesh_index,
                .distance = distance,
            };
        continue;
      }

      uint32_t first_instance = 0;
      if (!picking_render_submesh(rf, instance_pool, mesh, submesh,
                                  fallback_texture, can_alpha_test,
                                  &first_instance)) {
        continue;
      }

      vkr_geometry_system_render_instanced_range(
          rf, &rf->geometry_system, submesh->geometry, submesh->index_count,
          submesh->first_index, submesh->vertex_offset, 1, first_instance);
    }
  }

  if (has_transparent_pipeline && transparent_entries &&
      transparent_count > 0) {
    qsort(transparent_entries, transparent_count,
          sizeof(VkrPickingTransparentSubmeshEntry),
          vkr_picking_transparent_submesh_compare);

    VkrRendererError transparent_bind_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry,
                                        ctx->picking_transparent_pipeline,
                                        &transparent_bind_err);
    if (transparent_bind_err == VKR_RENDERER_ERROR_NONE) {
      vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                       VKR_PIPELINE_DOMAIN_PICKING);

      vkr_shader_system_bind_instance(&rf->shader_system,
                                      ctx->mesh_transparent_instance_state.id);

      for (uint32_t t = 0; t < transparent_count; ++t) {
        VkrPickingTransparentSubmeshEntry *entry = &transparent_entries[t];
        VkrMesh *mesh = vkr_mesh_manager_get(mesh_manager, entry->mesh_index);
        if (!mesh || !mesh->visible) {
          continue;
        }

        VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(
            mesh_manager, entry->mesh_index, entry->submesh_index);
        if (!submesh) {
          continue;
        }

        uint32_t first_instance = 0;
        if (!picking_render_submesh(rf, instance_pool, mesh, submesh,
                                    fallback_texture, true_v,
                                    &first_instance)) {
          continue;
        }

        vkr_geometry_system_render_instanced_range(
            rf, &rf->geometry_system, submesh->geometry, submesh->index_count,
            submesh->first_index, submesh->vertex_offset, 1, first_instance);
      }
    }

    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  } else if (vkr_allocator_scope_is_valid(&temp_scope)) {
    vkr_allocator_end_scope(&temp_scope, VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  if (rf->gizmo_system.initialized && rf->gizmo_system.visible &&
      ctx->picking_overlay_pipeline.id != 0) {
    if (ctx->shader_config.instance_texture_count > 0 &&
        ctx->mesh_overlay_instance_state.id == VKR_INVALID_ID) {
      VkrRendererError instance_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_pipeline_registry_acquire_instance_state(
              &rf->pipeline_registry, ctx->picking_overlay_pipeline,
              &ctx->mesh_overlay_instance_state, &instance_err)) {
        log_warn("Failed to re-acquire overlay picking instance state");
      }
    }

    if (ctx->mesh_overlay_instance_state.id != VKR_INVALID_ID) {
      VkrRendererError overlay_bind_err = VKR_RENDERER_ERROR_NONE;
      vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry,
                                          ctx->picking_overlay_pipeline,
                                          &overlay_bind_err);
      if (overlay_bind_err == VKR_RENDERER_ERROR_NONE) {
        vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                         VKR_PIPELINE_DOMAIN_PICKING);
        vkr_shader_system_bind_instance(&rf->shader_system,
                                        ctx->mesh_overlay_instance_state.id);

        VkrCamera *camera = vkr_camera_registry_get_by_handle(
            &rf->camera_system, rf->active_camera);
        vkr_gizmo_system_render_picking(&rf->gizmo_system, rf, camera,
                                        ctx->height);
      }
    }
  }

  // Render light gizmos for picking (uses active scene from frontend)
  if (rf->active_scene) {
    vkr_picking_render_light_gizmos(rf, ctx, rf->active_scene);
  }

  if (ctx->picking_text_pipeline.id != 0 &&
      ctx->picking_world_text_pipeline.id != 0) {
    // Draw WORLD picking text first (depth-tested), then UI picking text last.
    vkr_view_world_render_picking_text(rf, ctx->picking_world_text_pipeline);
    vkr_view_ui_render_picking_text(rf, ctx->picking_text_pipeline);
  }

  VkrRendererError end_err = vkr_renderer_end_render_pass(rf);
  if (end_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(end_err);
    log_error("Failed to end picking render pass: %s", string8_cstr(&err_str));
    ctx->state = VKR_PICKING_STATE_IDLE;
    return;
  }

  VkrRendererError readback_err = vkr_renderer_request_pixel_readback(
      rf, ctx->picking_texture, ctx->requested_x, ctx->requested_y);
  if (readback_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(readback_err);
    log_error("Failed to request pixel readback: %s", string8_cstr(&err_str));
    ctx->state = VKR_PICKING_STATE_IDLE;
    return;
  }

  ctx->state = VKR_PICKING_STATE_READBACK_PENDING;
}

VkrPickResult vkr_picking_get_result(struct s_RendererFrontend *renderer,
                                     VkrPickingContext *ctx) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(ctx != NULL, "Picking context is NULL");

  VkrPickResult result = {.object_id = 0, .hit = false_v};

  if (!ctx->initialized) {
    return result;
  }

  // Cache the result for the next call
  result.object_id = ctx->result_object_id;
  result.hit = (ctx->result_object_id > 0);

  if (ctx->state == VKR_PICKING_STATE_RESULT_READY) {
    result.object_id = ctx->result_object_id;
    result.hit = (ctx->result_object_id > 0);
    ctx->state = VKR_PICKING_STATE_IDLE;
    return result;
  }

  if (ctx->state != VKR_PICKING_STATE_READBACK_PENDING) {
    return result;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;

  VkrPixelReadbackResult readback_result = {0};
  VkrRendererError poll_err =
      vkr_renderer_get_pixel_readback_result(rf, &readback_result);

  if (poll_err != VKR_RENDERER_ERROR_NONE) {
    String8 err_str = vkr_renderer_get_error_string(poll_err);
    log_error("Failed to get pixel readback result: %s",
              string8_cstr(&err_str));
    ctx->state = VKR_PICKING_STATE_IDLE;
    return result;
  }

  switch (readback_result.status) {
  case VKR_READBACK_STATUS_READY:
    if (readback_result.valid) {
      result.object_id = readback_result.data;
      result.hit = (result.object_id > 0);
      ctx->result_object_id = result.object_id;
    }
    ctx->state = VKR_PICKING_STATE_IDLE;
    break;

  case VKR_READBACK_STATUS_PENDING:
    // Still waiting for GPU
    break;

  case VKR_READBACK_STATUS_ERROR:
    log_error("Pixel readback error");
    ctx->state = VKR_PICKING_STATE_IDLE;
    break;

  case VKR_READBACK_STATUS_IDLE:
    log_warn("Readback status IDLE when expecting PENDING");
    ctx->state = VKR_PICKING_STATE_IDLE;
    break;
  }

  return result;
}

bool8_t vkr_picking_is_pending(const VkrPickingContext *ctx) {
  if (!ctx) {
    return false_v;
  }
  return ctx->state == VKR_PICKING_STATE_RENDER_PENDING ||
         ctx->state == VKR_PICKING_STATE_READBACK_PENDING;
}

void vkr_picking_cancel(VkrPickingContext *ctx) {
  if (!ctx) {
    return;
  }
  ctx->state = VKR_PICKING_STATE_IDLE;
  ctx->result_object_id = 0;
}

void vkr_picking_invalidate_instance_states(struct s_RendererFrontend *renderer,
                                            VkrPickingContext *ctx) {
  if (!renderer || !ctx || !ctx->initialized) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;

  // Release instance states to invalidate descriptor sets that may reference
  // destroyed textures. New states will be acquired on next picking render.
  picking_release_instance_state(rf, ctx->picking_pipeline,
                                 &ctx->mesh_instance_state);
  picking_release_instance_state(rf, ctx->picking_overlay_pipeline,
                                 &ctx->mesh_overlay_instance_state);
  picking_release_instance_state(rf, ctx->picking_transparent_pipeline,
                                 &ctx->mesh_transparent_instance_state);

  log_debug("Picking instance states invalidated");
}

// ============================================================================
// Light Gizmo Picking
// ============================================================================

/** Size of light gizmo cube in world units. */
#define VKR_LIGHT_GIZMO_SIZE 0.25f

/**
 * @brief Context for iterating point lights during picking render.
 */
typedef struct LightGizmoPickingContext {
  RendererFrontend *rf;
  VkrPickingContext *ctx;
  const VkrScene *scene;
} LightGizmoPickingContext;

/**
 * @brief Chunk callback for rendering point light gizmos.
 */
vkr_internal void picking_render_point_light_cb(const VkrArchetype *arch,
                                                VkrChunk *chunk, void *user) {
  (void)arch;
  LightGizmoPickingContext *lctx = (LightGizmoPickingContext *)user;
  RendererFrontend *rf = lctx->rf;
  VkrPickingContext *ctx = lctx->ctx;
  const VkrScene *scene = lctx->scene;

  uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);

  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  ScenePointLight *lights = (ScenePointLight *)vkr_entity_chunk_column(
      chunk, scene->comp_point_light);

  if (!transforms || !lights)
    return;

  float32_t alpha_cutoff = 0.0f;
  VkrInstanceBufferPool *instance_pool = &rf->instance_buffer_pool;
  if (!instance_pool->initialized) {
    log_error("Picking render requires an initialized instance buffer pool");
    return;
  }

  for (uint32_t i = 0; i < count; i++) {
    if (!lights[i].enabled)
      continue;

    // Get render ID for picking (skip if not assigned)
    const SceneRenderId *render_id =
        (const SceneRenderId *)vkr_entity_get_component(
            scene->world, entities[i], scene->comp_render_id);
    if (!render_id || render_id->id == 0)
      continue;

    // Get world position from transform
    Vec3 world_pos = mat4_position(transforms[i].world);

    // Build model matrix: translate to position and scale to gizmo size
    Mat4 model =
        mat4_mul(mat4_translate(world_pos),
                 mat4_scale((Vec3){VKR_LIGHT_GIZMO_SIZE, VKR_LIGHT_GIZMO_SIZE,
                                   VKR_LIGHT_GIZMO_SIZE}));

    // Encode picking ID using SCENE kind (reuses existing mapping)
    uint32_t object_id =
        vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE, render_id->id);

    VkrInstanceDataGPU *instance_ptr = NULL;
    uint32_t base_instance = 0;
    if (!vkr_instance_buffer_alloc(instance_pool, 1, &base_instance,
                                   &instance_ptr)) {
      continue;
    }

    *instance_ptr = (VkrInstanceDataGPU){
        .model = model,
        .object_id = object_id,
        .material_index = 0,
        .flags = 0,
        ._padding = 0,
    };
    vkr_instance_buffer_flush_range(instance_pool, base_instance, 1);

    vkr_shader_system_uniform_set(&rf->shader_system, "alpha_cutoff",
                                  &alpha_cutoff);

    if (!vkr_shader_system_apply_instance(&rf->shader_system))
      continue;

    // Draw the cube
    vkr_geometry_system_render_instanced(
        rf, &rf->geometry_system, ctx->light_gizmo_cube, 1, base_instance);
  }
}

void vkr_picking_render_light_gizmos(struct s_RendererFrontend *renderer,
                                     VkrPickingContext *ctx,
                                     const struct VkrScene *scene) {
  if (!renderer || !ctx || !ctx->initialized)
    return;

  // Skip if no scene or light gizmo cube not available
  if (!scene || !scene->world || !scene->queries_valid)
    return;

  if (!ctx->light_gizmo_cube.id)
    return;

  RendererFrontend *rf = (RendererFrontend *)renderer;

  // Iterate point lights with the compiled query
  LightGizmoPickingContext lctx = {
      .rf = rf,
      .ctx = ctx,
      .scene = scene,
  };

  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_point_lights,
      picking_render_point_light_cb, &lctx);
}

void vkr_picking_shutdown(struct s_RendererFrontend *renderer,
                          VkrPickingContext *ctx) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(ctx != NULL, "Picking context is NULL");

  if (!ctx->initialized) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;

  if (vkr_renderer_wait_idle(rf) != VKR_RENDERER_ERROR_NONE) {
    log_error("Failed to wait for renderer to be idle");
    return;
  }

  picking_release_instance_state(rf, ctx->picking_pipeline,
                                 &ctx->mesh_instance_state);
  picking_release_instance_state(rf, ctx->picking_overlay_pipeline,
                                 &ctx->mesh_overlay_instance_state);
  picking_release_instance_state(rf, ctx->picking_transparent_pipeline,
                                 &ctx->mesh_transparent_instance_state);

  if (ctx->picking_pipeline.id != 0) {
    vkr_pipeline_registry_release(&rf->pipeline_registry,
                                  ctx->picking_pipeline);
    ctx->picking_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (ctx->picking_overlay_pipeline.id != 0) {
    vkr_pipeline_registry_release(&rf->pipeline_registry,
                                  ctx->picking_overlay_pipeline);
    ctx->picking_overlay_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (ctx->picking_transparent_pipeline.id != 0) {
    vkr_pipeline_registry_release(&rf->pipeline_registry,
                                  ctx->picking_transparent_pipeline);
    ctx->picking_transparent_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (ctx->picking_text_pipeline.id != 0) {
    vkr_pipeline_registry_release(&rf->pipeline_registry,
                                  ctx->picking_text_pipeline);
    ctx->picking_text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  if (ctx->picking_world_text_pipeline.id != 0) {
    vkr_pipeline_registry_release(&rf->pipeline_registry,
                                  ctx->picking_world_text_pipeline);
    ctx->picking_world_text_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  picking_destroy_attachments(rf, ctx);

  // Release light gizmo cube
  if (ctx->light_gizmo_cube.id) {
    vkr_geometry_system_release(&rf->geometry_system, ctx->light_gizmo_cube);
    ctx->light_gizmo_cube = (VkrGeometryHandle){0};
  }

  // Note: render pass is shared/cached, don't destroy it here

  ctx->initialized = false_v;
  ctx->state = VKR_PICKING_STATE_IDLE;

  log_info("Picking system shutdown");
}
