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
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_material_system.h"
#include "renderer/systems/vkr_mesh_manager.h"
#include "renderer/systems/vkr_pipeline_registry.h"
#include "renderer/systems/vkr_resource_system.h"
#include "renderer/systems/vkr_scene_system.h"
#include "renderer/systems/vkr_shader_system.h"
#include "renderer/systems/vkr_ui_system.h"
#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vulkan/vulkan_types.h"
#include "vkr_picking_ids.h"
#include "vkr_picking_system.h"

vkr_internal VkrTextureFormat
vkr_picking_get_depth_format(RendererFrontend *rf) {
  return rf ? vkr_renderer_present_target_format(
                  rf, VKR_PRESENT_TARGET_ATTACHMENT_DEPTH)
            : VKR_TEXTURE_FORMAT_D32_SFLOAT;
}

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
  bool8_t is_instance; // True if mesh_index refers to instance slot
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
 * @brief Records the picking target size.
 *
 * The colour and depth targets themselves are graph resources now
 * (`picking_color` / `picking_depth` in the render graph JSON), sized from the
 * viewport extent. The picking system keeps only the dimensions, which it needs
 * to bounds-check requested pick coordinates.
 */
vkr_internal bool8_t picking_set_target_size(VkrPickingContext *ctx,
                                             uint32_t width, uint32_t height) {
  assert_log(ctx != NULL, "Picking context is NULL");

  if (width == 0 || height == 0) {
    log_error("Invalid picking dimensions: %ux%u", width, height);
    return false_v;
  }

  ctx->width = width;
  ctx->height = height;
  return true_v;
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
 * @brief Rewind a pool's per-frame cursor when frame_number advances.
 */
vkr_internal void
picking_instance_pool_begin_frame(VkrPickingInstanceStatePool *pool,
                                  uint64_t frame_number) {
  if (!pool) {
    return;
  }
  if (pool->cursor_frame_number != frame_number) {
    pool->cursor_frame_number = frame_number;
    pool->cursor = 0;
  }
}

/**
 * @brief Grow pool storage while preserving acquired state handles.
 */
vkr_internal bool8_t picking_instance_pool_ensure_capacity(
    RendererFrontend *rf, VkrPickingInstanceStatePool *pool,
    uint32_t required_capacity) {
  if (!rf || !pool) {
    return false_v;
  }
  if (required_capacity <= pool->capacity) {
    return true_v;
  }

  uint32_t new_capacity = pool->capacity > 0 ? pool->capacity * 2u : 64u;
  while (new_capacity < required_capacity) {
    new_capacity *= 2u;
  }

  VkrRendererInstanceStateHandle *new_states = vkr_allocator_alloc(
      &rf->allocator, sizeof(VkrRendererInstanceStateHandle) * new_capacity,
      VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!new_states) {
    log_error("Picking: failed to grow alpha instance pool to %u",
              new_capacity);
    return false_v;
  }

  for (uint32_t i = 0; i < new_capacity; ++i) {
    new_states[i].id = VKR_INVALID_ID;
  }

  if (pool->states && pool->count > 0) {
    MemCopy(new_states, pool->states,
            sizeof(VkrRendererInstanceStateHandle) * (uint64_t)pool->count);
  }

  if (pool->states) {
    vkr_allocator_free(&rf->allocator, pool->states,
                       sizeof(VkrRendererInstanceStateHandle) *
                           (uint64_t)pool->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }

  pool->states = new_states;
  pool->capacity = new_capacity;
  return true_v;
}

/**
 * @brief Return the next per-draw alpha instance state, acquiring as needed.
 */
vkr_internal bool8_t picking_instance_pool_acquire_next(
    RendererFrontend *rf, VkrPipelineHandle pipeline,
    VkrPickingInstanceStatePool *pool,
    VkrRendererInstanceStateHandle *out_state) {
  if (!rf || !pool || !out_state || pipeline.id == 0) {
    return false_v;
  }

  const uint32_t slot = pool->cursor;
  if (!picking_instance_pool_ensure_capacity(rf, pool, slot + 1u)) {
    return false_v;
  }

  if (slot >= pool->count) {
    VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
    VkrRendererInstanceStateHandle state_handle = {.id = VKR_INVALID_ID};
    if (!vkr_pipeline_registry_acquire_instance_state(
            &rf->pipeline_registry, pipeline, &state_handle, &acquire_err)) {
      String8 err = vkr_renderer_get_error_string(acquire_err);
      log_warn("Picking: failed to acquire alpha instance state: %s",
               string8_cstr(&err));
      return false_v;
    }

    pool->states[slot] = state_handle;
    pool->count = slot + 1u;
  }

  *out_state = pool->states[slot];
  pool->cursor++;
  return true_v;
}

/**
 * @brief Release all acquired handles in the pool but keep storage cached.
 */
vkr_internal void
picking_instance_pool_release_states(RendererFrontend *rf,
                                     VkrPipelineHandle pipeline,
                                     VkrPickingInstanceStatePool *pool) {
  if (!rf || !pool || pipeline.id == 0) {
    return;
  }

  for (uint32_t i = 0; i < pool->count; ++i) {
    picking_release_instance_state(rf, pipeline, &pool->states[i]);
  }

  pool->count = 0;
  pool->cursor = 0;
  pool->cursor_frame_number = UINT64_MAX;
}

/**
 * @brief Release all handles and free pool storage.
 */
vkr_internal void
picking_instance_pool_destroy(RendererFrontend *rf, VkrPipelineHandle pipeline,
                              VkrPickingInstanceStatePool *pool) {
  if (!rf || !pool) {
    return;
  }

  picking_instance_pool_release_states(rf, pipeline, pool);

  if (pool->states) {
    vkr_allocator_free(&rf->allocator, pool->states,
                       sizeof(VkrRendererInstanceStateHandle) *
                           (uint64_t)pool->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    pool->states = NULL;
  }

  pool->capacity = 0;
}

void vkr_picking_begin_frame_instance_pools(VkrPickingContext *ctx,
                                            uint64_t frame_number) {
  if (!ctx) {
    return;
  }
  picking_instance_pool_begin_frame(&ctx->mesh_alpha_instance_pool,
                                    frame_number);
  picking_instance_pool_begin_frame(&ctx->mesh_transparent_alpha_instance_pool,
                                    frame_number);
}

bool8_t vkr_picking_bind_draw_instance_state(
    struct s_RendererFrontend *renderer, VkrPipelineHandle pipeline,
    VkrRendererInstanceStateHandle *shared_state,
    VkrPickingInstanceStatePool *alpha_pool, bool8_t use_alpha_cutoff) {
  if (!renderer || pipeline.id == 0 || !shared_state || !alpha_pool) {
    return false_v;
  }

  RendererFrontend *rf = (RendererFrontend *)renderer;
  VkrRendererInstanceStateHandle bound_state = {.id = VKR_INVALID_ID};

  if (use_alpha_cutoff) {
    if (!picking_instance_pool_acquire_next(rf, pipeline, alpha_pool,
                                            &bound_state)) {
      return false_v;
    }
  } else {
    if (shared_state->id == VKR_INVALID_ID) {
      VkrRendererError acquire_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_pipeline_registry_acquire_instance_state(
              &rf->pipeline_registry, pipeline, shared_state, &acquire_err)) {
        String8 err = vkr_renderer_get_error_string(acquire_err);
        log_warn("Picking: failed to acquire shared instance state: %s",
                 string8_cstr(&err));
        shared_state->id = VKR_INVALID_ID;
        return false_v;
      }
    }
    bound_state = *shared_state;
  }

  if (bound_state.id == VKR_INVALID_ID) {
    return false_v;
  }
  return vkr_shader_system_bind_instance(&rf->shader_system, bound_state.id);
}

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Resolve per-draw alpha-cutout state for picking.
 *
 * When alpha testing is disabled (or no valid cutout material is present),
 * output uses fallback texture and zero cutoff so descriptor updates stay
 * stable for shared non-alpha states.
 */
vkr_internal void picking_resolve_draw_alpha_state(
    RendererFrontend *rf, VkrMaterialHandle material_handle,
    VkrTextureOpaqueHandle fallback_texture, bool8_t can_alpha_test,
    float32_t *out_alpha_cutoff, VkrTextureOpaqueHandle *out_diffuse_texture,
    bool8_t *out_use_alpha_cutoff) {
  assert_log(out_alpha_cutoff != NULL, "out_alpha_cutoff is NULL");
  assert_log(out_diffuse_texture != NULL, "out_diffuse_texture is NULL");
  assert_log(out_use_alpha_cutoff != NULL, "out_use_alpha_cutoff is NULL");

  *out_alpha_cutoff = 0.0f;
  *out_diffuse_texture = fallback_texture;
  *out_use_alpha_cutoff = false_v;

  if (!rf || !can_alpha_test || material_handle.id == 0) {
    return;
  }

  VkrMaterial *material =
      vkr_material_system_get_by_handle(&rf->material_system, material_handle);
  if (!material) {
    return;
  }

  const float32_t alpha_cutoff =
      vkr_material_system_material_alpha_cutoff(&rf->material_system, material);
  if (alpha_cutoff <= 0.0f) {
    return;
  }

  VkrTextureOpaqueHandle diffuse_texture = fallback_texture;
  VkrMaterialTexture *diffuse_tex =
      &material->textures[VKR_TEXTURE_SLOT_DIFFUSE];
  if (diffuse_tex->enabled && diffuse_tex->handle.id != 0) {
    VkrTexture *texture = vkr_texture_system_get_by_handle(&rf->texture_system,
                                                           diffuse_tex->handle);
    if (texture && texture->handle &&
        texture->description.type == VKR_TEXTURE_TYPE_2D) {
      diffuse_texture = texture->handle;
    }
  }

  *out_alpha_cutoff = alpha_cutoff;
  *out_diffuse_texture = diffuse_texture;
  *out_use_alpha_cutoff = true_v;
}

/**
 * @brief Upload one picking instance payload and flush immediately.
 */
vkr_internal bool8_t picking_upload_instance_payload(
    VkrInstanceBufferPool *instance_pool, Mat4 model, uint32_t object_id,
    uint32_t *out_first_instance) {
  if (!instance_pool || !out_first_instance) {
    return false_v;
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
  };
  vkr_instance_buffer_flush_range(instance_pool, base_instance, 1);
  *out_first_instance = base_instance;
  return true_v;
}

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
    VkrSubMesh *submesh, VkrPipelineHandle pipeline,
    VkrRendererInstanceStateHandle *shared_state,
    VkrPickingInstanceStatePool *alpha_pool,
    VkrTextureOpaqueHandle fallback_texture, bool8_t can_alpha_test,
    uint32_t *out_first_instance) {
  if (!rf || !instance_pool || !mesh || !submesh || pipeline.id == 0 ||
      !shared_state || !alpha_pool || !out_first_instance) {
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

  float32_t alpha_cutoff = 0.0f;
  VkrTextureOpaqueHandle diffuse_texture_handle = fallback_texture;
  bool8_t use_alpha_cutoff = false_v;
  picking_resolve_draw_alpha_state(rf, submesh->material, fallback_texture,
                                   can_alpha_test, &alpha_cutoff,
                                   &diffuse_texture_handle, &use_alpha_cutoff);

  uint32_t base_instance = 0;
  if (!picking_upload_instance_payload(instance_pool, model, object_id,
                                       &base_instance)) {
    return false_v;
  }

  if (!vkr_picking_bind_draw_instance_state(rf, pipeline, shared_state,
                                            alpha_pool, use_alpha_cutoff)) {
    return false_v;
  }

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

vkr_internal bool8_t picking_render_instance_submesh(
    RendererFrontend *rf, VkrInstanceBufferPool *instance_pool,
    VkrMeshInstance *instance, VkrMeshAssetSubmesh *submesh,
    VkrPipelineHandle pipeline, VkrRendererInstanceStateHandle *shared_state,
    VkrPickingInstanceStatePool *alpha_pool,
    VkrTextureOpaqueHandle fallback_texture, bool8_t can_alpha_test,
    uint32_t *out_first_instance) {
  if (!rf || !instance_pool || !instance || !submesh || pipeline.id == 0 ||
      !shared_state || !alpha_pool || !out_first_instance) {
    return false_v;
  }

  if (!instance_pool->initialized) {
    return false_v;
  }

  Mat4 model = instance->model;
  uint32_t object_id = instance->render_id
                           ? vkr_picking_encode_id(VKR_PICKING_ID_KIND_SCENE,
                                                   instance->render_id)
                           : 0;

  float32_t alpha_cutoff = 0.0f;
  VkrTextureOpaqueHandle diffuse_texture_handle = fallback_texture;
  bool8_t use_alpha_cutoff = false_v;
  picking_resolve_draw_alpha_state(rf, submesh->material, fallback_texture,
                                   can_alpha_test, &alpha_cutoff,
                                   &diffuse_texture_handle, &use_alpha_cutoff);

  uint32_t base_instance = 0;
  if (!picking_upload_instance_payload(instance_pool, model, object_id,
                                       &base_instance)) {
    return false_v;
  }

  if (!vkr_picking_bind_draw_instance_state(rf, pipeline, shared_state,
                                            alpha_pool, use_alpha_cutoff)) {
    return false_v;
  }

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
  ctx->mesh_alpha_instance_pool.cursor_frame_number = UINT64_MAX;
  ctx->mesh_transparent_alpha_instance_pool.cursor_frame_number = UINT64_MAX;

  if (width == 0 || height == 0) {
    log_error("Invalid picking dimensions: %ux%u", width, height);
    return false_v;
  }

  if (!rf->impl.caps.uses_legacy_pipeline_state) {
    if (!picking_set_target_size(ctx, width, height)) {
      return false_v;
    }
    ctx->initialized = true_v;
    return true_v;
  }

  ctx->picking_pass = vkr_renderer_renderpass_get(
      rf, string8_lit("Renderpass.Builtin.Picking"));
  if (!ctx->picking_pass) {
    VkrTextureFormat depth_format = vkr_picking_get_depth_format(rf);
    VkrClearValue clear_picking = {.color_u32 = {0u, 0u, 0u, 0u}};
    VkrClearValue clear_depth = {.depth_stencil = {1.0f, 0}};
    VkrRenderPassAttachmentDesc picking_color = {
        .format = VKR_TEXTURE_FORMAT_R32_UINT,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_STORE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
        .final_layout = VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .clear_value = clear_picking,
    };
    VkrRenderPassAttachmentDesc picking_depth = {
        .format = depth_format,
        .samples = VKR_SAMPLE_COUNT_1,
        .load_op = VKR_ATTACHMENT_LOAD_OP_CLEAR,
        .stencil_load_op = VKR_ATTACHMENT_LOAD_OP_DONT_CARE,
        .store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencil_store_op = VKR_ATTACHMENT_STORE_OP_DONT_CARE,
        .initial_layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
        .final_layout = VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .clear_value = clear_depth,
    };
    VkrRenderPassDesc pass_desc = {
        .name = string8_lit("Renderpass.Builtin.Picking"),
        .domain = VKR_PIPELINE_DOMAIN_PICKING,
        .color_attachment_count = 1,
        .color_attachments = &picking_color,
        .depth_stencil_attachment = &picking_depth,
        .resolve_attachment_count = 0,
        .resolve_attachments = NULL,
    };
    VkrRendererError pass_err = VKR_RENDERER_ERROR_NONE;
    ctx->picking_pass =
        vkr_renderer_renderpass_create_desc(rf, &pass_desc, &pass_err);
    if (!ctx->picking_pass) {
      String8 err = vkr_renderer_get_error_string(pass_err);
      log_error("Failed to create picking render pass");
      log_error("Renderpass error: %s", string8_cstr(&err));
      return false_v;
    }
  }

  if (!picking_set_target_size(ctx, width, height)) {
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
    return false_v;
  }

  if (!cfg_info.as.custom) {
    log_error("Shader config returned null custom data");
    return false_v;
  }

  ctx->shader_config = *(VkrShaderConfig *)cfg_info.as.custom;

  if (!vkr_shader_system_create(&rf->shader_system, &ctx->shader_config)) {
    log_error("Failed to create picking shader in shader system");
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

  // Only bookkeeping: the graph owns picking_color/picking_depth and
  // reallocates them itself when the viewport extent changes, so there is
  // nothing here to destroy and no reason to idle the device.
  (void)renderer;
  if (!picking_set_target_size(ctx, new_width, new_height)) {
    ctx->initialized = false_v;
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
         ctx->state == VKR_PICKING_STATE_RENDER_RECORDED ||
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
  picking_instance_pool_release_states(rf, ctx->picking_pipeline,
                                       &ctx->mesh_alpha_instance_pool);
  picking_instance_pool_release_states(
      rf, ctx->picking_transparent_pipeline,
      &ctx->mesh_transparent_alpha_instance_pool);

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
  if (!vkr_shader_system_use(&rf->shader_system, "shader.picking")) {
    return;
  }

  VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry,
                                           ctx->picking_pipeline, &bind_err)) {
    return;
  }

  if (ctx->shader_config.instance_texture_count > 0 &&
      ctx->mesh_instance_state.id == VKR_INVALID_ID) {
    VkrRendererError instance_err = VKR_RENDERER_ERROR_NONE;
    (void)vkr_pipeline_registry_acquire_instance_state(
        &rf->pipeline_registry, ctx->picking_pipeline,
        &ctx->mesh_instance_state, &instance_err);
  }

  if (ctx->mesh_instance_state.id == VKR_INVALID_ID) {
    return;
  }

  vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                   VKR_PIPELINE_DOMAIN_PICKING);
  vkr_shader_system_bind_instance(&rf->shader_system,
                                  ctx->mesh_instance_state.id);
  VkrTexture *fallback_texture =
      vkr_texture_system_get_default(&rf->texture_system);
  if (fallback_texture && fallback_texture->handle) {
    vkr_shader_system_sampler_set(&rf->shader_system, "diffuse_texture",
                                  fallback_texture->handle);
  }

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
  picking_instance_pool_destroy(rf, ctx->picking_pipeline,
                                &ctx->mesh_alpha_instance_pool);
  picking_instance_pool_destroy(rf, ctx->picking_transparent_pipeline,
                                &ctx->mesh_transparent_alpha_instance_pool);

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
