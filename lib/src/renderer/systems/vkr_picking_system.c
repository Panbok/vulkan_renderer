/**
 * @file vkr_picking_system.c
 * @brief Implementation of the picking system for pixel-perfect object
 * selection.
 */

#include "vkr_picking_system.h"
#include "core/logger.h"
#include "defines.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_shader_system.h"

// ============================================================================
// Internal helpers
// ============================================================================

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

bool8_t vkr_picking_init(struct s_RendererFrontend *renderer,
                         VkrPickingContext *ctx, uint32_t width,
                         uint32_t height) {
  assert_log(renderer != NULL, "Renderer is NULL");
  assert_log(ctx != NULL, "Picking context is NULL");

  RendererFrontend *rf = (RendererFrontend *)renderer;
  MemZero(ctx, sizeof(VkrPickingContext));

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
    picking_destroy_attachments(rf, ctx);
    return false_v;
  }

  if (ctx->shader_config.name.str && ctx->shader_config.name.length > 0) {
    VkrRendererError alias_err = VKR_RENDERER_ERROR_NONE;
    vkr_pipeline_registry_alias_pipeline_name(
        &rf->pipeline_registry, ctx->picking_pipeline, ctx->shader_config.name,
        &alias_err);
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

  uint32_t mesh_capacity = vkr_mesh_manager_capacity(mesh_manager);

  for (uint32_t mesh_index = 0; mesh_index < mesh_capacity; mesh_index++) {
    VkrMesh *mesh = vkr_mesh_manager_get(mesh_manager, mesh_index);
    if (!mesh) {
      continue;
    }

    uint32_t submesh_count = vkr_mesh_manager_submesh_count(mesh);
    if (submesh_count == 0) {
      continue;
    }

    Mat4 model = vkr_transform_get_world(&mesh->transform);

    for (uint32_t submesh_index = 0; submesh_index < submesh_count;
         submesh_index++) {
      VkrSubMesh *submesh =
          vkr_mesh_manager_get_submesh(mesh_manager, mesh_index, submesh_index);
      if (!submesh) {
        continue;
      }

      vkr_material_system_apply_local(
          &rf->material_system,
          &(VkrLocalMaterialState){
              .model = model,
              .object_id = mesh_index + 1, // 0 = background
          });

      vkr_shader_system_apply_instance(&rf->shader_system);

      vkr_geometry_system_render(rf, &rf->geometry_system, submesh->geometry,
                                 1);
    }
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

  if (ctx->picking_pipeline.id != 0) {
    vkr_pipeline_registry_release(&rf->pipeline_registry,
                                  ctx->picking_pipeline);
    ctx->picking_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  }

  picking_destroy_attachments(rf, ctx);

  // Note: render pass is shared/cached, don't destroy it here

  ctx->initialized = false_v;
  ctx->state = VKR_PICKING_STATE_IDLE;

  log_info("Picking system shutdown");
}
