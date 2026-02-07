#include "renderer/passes/vkr_pass_picking.h"

#include "core/logger.h"
#include "renderer/passes/internal/vkr_pass_draw_utils.h"
#include "renderer/renderer_frontend.h"
#include "renderer/systems/vkr_gizmo_system.h"
#include "renderer/systems/vkr_picking_system.h"
#include "renderer/systems/vkr_ui_system.h"
#include "renderer/systems/vkr_world_resources.h"
#include "renderer/vkr_render_packet.h"

vkr_internal float32_t vkr_pass_picking_get_alpha_cutoff(
    const VkrMaterialSystem *system, const VkrMaterial *material) {
  if (!system || !material) {
    return 0.0f;
  }

  return vkr_material_system_material_alpha_cutoff(system, material);
}

vkr_internal VkrTextureOpaqueHandle vkr_pass_picking_get_diffuse_texture(
    RendererFrontend *rf, const VkrMaterial *material) {
  if (!rf) {
    return NULL;
  }

  VkrTextureHandle diffuse_handle =
      vkr_texture_system_get_default_diffuse_handle(&rf->texture_system);
  if (material && material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled) {
    diffuse_handle = material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle;
  }

  VkrTexture *diffuse =
      vkr_texture_system_get_by_handle(&rf->texture_system, diffuse_handle);
  if (!diffuse || diffuse->description.type != VKR_TEXTURE_TYPE_2D) {
    VkrTextureHandle fallback =
        vkr_texture_system_get_default_diffuse_handle(&rf->texture_system);
    diffuse = vkr_texture_system_get_by_handle(&rf->texture_system, fallback);
  }

  return diffuse ? diffuse->handle : NULL;
}

vkr_internal bool8_t vkr_pass_picking_bind_pipeline(
    RendererFrontend *rf, VkrPipelineHandle pipeline,
    const VkrFrameGlobals *globals) {
  if (!rf || pipeline.id == 0 || !globals) {
    return false_v;
  }

  VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
  if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, pipeline,
                                           &bind_err)) {
    return false_v;
  }

  vkr_shader_system_uniform_set(&rf->shader_system, "view", &globals->view);
  vkr_shader_system_uniform_set(&rf->shader_system, "projection",
                                &globals->projection);
  vkr_shader_system_apply_global(&rf->shader_system);
  return true_v;
}

vkr_internal void vkr_pass_picking_draw_list(RendererFrontend *rf,
                                             VkrPickingContext *picking,
                                             const VkrFrameGlobals *globals,
                                             const VkrDrawItem *draws,
                                             uint32_t draw_count,
                                             uint32_t base_instance) {
  if (!rf || !picking || !globals || !draws || draw_count == 0) {
    return;
  }

  if (!vkr_shader_system_use(&rf->shader_system, "shader.picking")) {
    log_error("Picking pass: failed to use shader.picking");
    return;
  }

  VkrPipelineHandle current_pipeline = VKR_PIPELINE_HANDLE_INVALID;
  bool8_t shared_instance_bound = false_v;
  VkrTextureOpaqueHandle default_diffuse =
      vkr_pass_picking_get_diffuse_texture(rf, NULL);

  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    if (draw->instance_count == 0) {
      continue;
    }

    VkrMaterialHandle material_handle = draw->material;
    VkrMeshAssetSubmesh *asset_submesh = NULL;
    VkrSubMesh *mesh_submesh = NULL;

    if (vkr_pass_packet_handle_is_instance(draw->mesh)) {
      VkrMeshInstance *instance = NULL;
      VkrMeshAsset *asset = NULL;
      VkrMeshSubmeshInstanceState *inst_state = NULL;
      if (!vkr_pass_packet_resolve_instance(rf, draw->mesh, draw->submesh_index,
                                            &instance, &asset, &asset_submesh,
                                            &inst_state)) {
        continue;
      }
      (void)instance;
      (void)asset;
      (void)inst_state;
      if (material_handle.id == 0) {
        material_handle = asset_submesh->material;
      }
    } else {
      VkrMesh *mesh = NULL;
      if (!vkr_pass_packet_resolve_mesh(rf, draw->mesh, draw->submesh_index,
                                        &mesh, &mesh_submesh)) {
        continue;
      }
      (void)mesh;
      if (material_handle.id == 0) {
        material_handle = mesh_submesh->material;
      }
    }

    VkrMaterial *material = vkr_material_system_get_by_handle(
        &rf->material_system, material_handle);
    if (!material && rf->material_system.default_material.id != 0) {
      material = vkr_material_system_get_by_handle(
          &rf->material_system, rf->material_system.default_material);
    }

    bool8_t requires_blend = vkr_material_system_material_has_transparency(
        &rf->material_system, material);
    float32_t alpha_cutoff =
        vkr_pass_picking_get_alpha_cutoff(&rf->material_system, material);
    bool8_t use_alpha = alpha_cutoff > 0.0f;
    bool8_t use_transparent_pipeline = requires_blend;

    VkrPipelineHandle pipeline = use_transparent_pipeline
                                     ? picking->picking_transparent_pipeline
                                     : picking->picking_pipeline;
    VkrRendererInstanceStateHandle *shared_state =
        use_transparent_pipeline ? &picking->mesh_transparent_instance_state
                                 : &picking->mesh_instance_state;
    VkrPickingInstanceStatePool *alpha_pool =
        use_transparent_pipeline ? &picking->mesh_transparent_alpha_instance_pool
                                 : &picking->mesh_alpha_instance_pool;

    if (pipeline.id == 0) {
      pipeline = picking->picking_pipeline;
      shared_state = &picking->mesh_instance_state;
      alpha_pool = &picking->mesh_alpha_instance_pool;
      use_alpha = false_v;
      alpha_cutoff = 0.0f;
    }

    if (pipeline.id == 0) {
      continue;
    }

    if (pipeline.id != current_pipeline.id ||
        pipeline.generation != current_pipeline.generation) {
      if (!vkr_pass_picking_bind_pipeline(rf, pipeline, globals)) {
        continue;
      }
      current_pipeline = pipeline;
      shared_instance_bound = false_v;
    }

    if (use_alpha) {
      if (!vkr_picking_bind_draw_instance_state(rf, pipeline, shared_state,
                                                alpha_pool, true_v)) {
        continue;
      }

      VkrTextureOpaqueHandle diffuse =
          vkr_pass_picking_get_diffuse_texture(rf, material);
      vkr_shader_system_uniform_set(&rf->shader_system, "alpha_cutoff",
                                    &alpha_cutoff);
      if (diffuse) {
        vkr_shader_system_sampler_set(&rf->shader_system, "diffuse_texture",
                                      diffuse);
      }
      if (!vkr_shader_system_apply_instance(&rf->shader_system)) {
        continue;
      }
      shared_instance_bound = false_v;
    } else if (!shared_instance_bound ||
               shared_state->id == VKR_INVALID_ID) {
      float32_t zero_cutoff = 0.0f;
      if (!vkr_picking_bind_draw_instance_state(rf, pipeline, shared_state,
                                                alpha_pool, false_v)) {
        continue;
      }
      vkr_shader_system_uniform_set(&rf->shader_system, "alpha_cutoff",
                                    &zero_cutoff);
      if (default_diffuse) {
        vkr_shader_system_sampler_set(&rf->shader_system, "diffuse_texture",
                                      default_diffuse);
      }
      if (!vkr_shader_system_apply_instance(&rf->shader_system)) {
        continue;
      }
      shared_instance_bound = true_v;
    }

    bool8_t use_opaque_range = !use_transparent_pipeline;
    VkrPassDrawRange range = {0};
    if (asset_submesh) {
      if (!vkr_pass_packet_resolve_draw_range(rf, asset_submesh,
                                              use_opaque_range, &range)) {
        continue;
      }
    } else if (mesh_submesh) {
      if (!vkr_pass_packet_resolve_draw_range_mesh(rf, mesh_submesh,
                                                   use_opaque_range, &range)) {
        continue;
      }
    } else {
      continue;
    }

    vkr_geometry_system_render_instanced_range_with_index_buffer(
        rf, &rf->geometry_system,
        asset_submesh ? asset_submesh->geometry : mesh_submesh->geometry,
        range.index_buffer, range.index_count, range.first_index,
        range.vertex_offset, draw->instance_count,
        base_instance + draw->first_instance);
  }
}

vkr_internal void vkr_pass_picking_execute(VkrRgPassContext *ctx,
                                           void *user_data) {
  (void)user_data;

  if (!ctx || !ctx->renderer) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)ctx->renderer;
  const VkrRenderPacket *packet = vkr_rg_pass_get_packet(ctx);
  const VkrPickingPassPayload *payload = vkr_rg_pass_get_picking_payload(ctx);
  if (!packet || !payload || !payload->pending) {
    return;
  }

  VkrPickingContext *picking = &rf->picking;
  if (!picking->initialized) {
    return;
  }

  vkr_picking_request(picking, payload->x, payload->y);
  if (picking->state != VKR_PICKING_STATE_RENDER_PENDING) {
    return;
  }
  vkr_picking_begin_frame_instance_pools(picking, rf->frame_number);

  const VkrDrawItem *draws = payload->draws;
  uint32_t draw_count = payload->draw_count;
  const VkrInstanceDataGPU *instances = payload->instances;
  uint32_t instance_count = payload->instance_count;

  const VkrDrawItem *transparent_draws = NULL;
  uint32_t transparent_count = 0;

  if (!draws) {
    if (!packet->world) {
      picking->state = VKR_PICKING_STATE_IDLE;
      return;
    }
    draws = packet->world->opaque_draws;
    draw_count = packet->world->opaque_draw_count;
    transparent_draws = packet->world->transparent_draws;
    transparent_count = packet->world->transparent_draw_count;
    instances = packet->world->instances;
    instance_count = packet->world->instance_count;
  }

  uint32_t base_instance = 0;
  if (!vkr_pass_packet_upload_instances(rf, instances, instance_count,
                                        &base_instance)) {
    picking->state = VKR_PICKING_STATE_IDLE;
    return;
  }

  VkrRendererError begin_err = vkr_renderer_begin_render_pass(
      rf, picking->picking_pass, picking->picking_target);
  if (begin_err != VKR_RENDERER_ERROR_NONE) {
    picking->state = VKR_PICKING_STATE_IDLE;
    return;
  }

  vkr_pass_picking_draw_list(rf, picking, &packet->globals, draws, draw_count,
                             base_instance);
  vkr_pass_picking_draw_list(rf, picking, &packet->globals, transparent_draws,
                             transparent_count, base_instance);

  if (rf->world_resources.initialized) {
    vkr_world_resources_render_picking_text(
        rf, &rf->world_resources, picking->picking_world_text_pipeline);
  }

  if (rf->gizmo_system.initialized && rf->gizmo_system.visible &&
      picking->picking_overlay_pipeline.id != 0) {
    if (picking->mesh_overlay_instance_state.id == VKR_INVALID_ID) {
      VkrRendererError inst_err = VKR_RENDERER_ERROR_NONE;
      (void)vkr_pipeline_registry_acquire_instance_state(
          &rf->pipeline_registry, picking->picking_overlay_pipeline,
          &picking->mesh_overlay_instance_state, &inst_err);
    }

    if (picking->mesh_overlay_instance_state.id != VKR_INVALID_ID) {
      if (vkr_shader_system_use(&rf->shader_system, "shader.picking")) {
        VkrRendererError overlay_bind_err = VKR_RENDERER_ERROR_NONE;
        if (vkr_pipeline_registry_bind_pipeline(
                &rf->pipeline_registry, picking->picking_overlay_pipeline,
                &overlay_bind_err)) {
          vkr_material_system_apply_global(&rf->material_system, &rf->globals,
                                           VKR_PIPELINE_DOMAIN_PICKING);
          vkr_shader_system_bind_instance(
              &rf->shader_system, picking->mesh_overlay_instance_state.id);
          VkrCamera *camera = vkr_camera_registry_get_by_handle(
              &rf->camera_system, rf->active_camera);
          if (camera) {
            vkr_gizmo_system_render_picking(&rf->gizmo_system, rf, camera,
                                            picking->height);
          }
        }
      }
    }
  }

  if (rf->ui_system.initialized) {
    vkr_ui_system_render_picking_text(rf, &rf->ui_system,
                                      picking->picking_text_pipeline);
  }

  vkr_renderer_end_render_pass(rf);

  VkrRendererError readback_err = vkr_renderer_request_pixel_readback(
      rf, picking->picking_texture, picking->requested_x, picking->requested_y);
  if (readback_err != VKR_RENDERER_ERROR_NONE) {
    picking->state = VKR_PICKING_STATE_IDLE;
    return;
  }

  picking->state = VKR_PICKING_STATE_READBACK_PENDING;
}

bool8_t vkr_pass_picking_register(VkrRgExecutorRegistry *registry) {
  if (!registry) {
    return false_v;
  }

  VkrRgPassExecutor entry = {
      .name = string8_lit("pass.picking"),
      .execute = vkr_pass_picking_execute,
      .user_data = NULL,
  };

  return vkr_rg_executor_registry_register(registry, &entry);
}
