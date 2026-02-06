#include "renderer/passes/vkr_pass_editor.h"

#include "math/mat.h"
#include "renderer/passes/internal/vkr_pass_draw_utils.h"
#include "renderer/renderer_frontend.h"
#include "renderer/vkr_render_packet.h"

vkr_internal void vkr_pass_editor_draw_list(
    RendererFrontend *rf, const VkrEditorPassPayload *payload,
    const VkrDrawItem *draws, uint32_t draw_count, uint32_t base_instance,
    const VkrGlobalMaterialState *globals) {
  if (!rf || !payload || !draws || draw_count == 0) {
    return;
  }

  VkrPipelineHandle globals_pipeline = VKR_PIPELINE_HANDLE_INVALID;

  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    if (draw->instance_count == 0) {
      continue;
    }

    VkrMeshAssetSubmesh *asset_submesh = NULL;
    VkrSubMesh *mesh_submesh = NULL;
    VkrMeshSubmeshInstanceState *inst_state = NULL;

    bool8_t is_instance = vkr_pass_packet_handle_is_instance(draw->mesh);
    if (is_instance) {
      VkrMeshInstance *instance = NULL;
      VkrMeshAsset *asset = NULL;
      if (!vkr_pass_packet_resolve_instance(rf, draw->mesh, draw->submesh_index,
                                            &instance, &asset, &asset_submesh,
                                            &inst_state)) {
        continue;
      }
      (void)instance;
      (void)asset;
    } else {
      VkrMesh *mesh = NULL;
      if (!vkr_pass_packet_resolve_mesh(rf, draw->mesh, draw->submesh_index,
                                        &mesh, &mesh_submesh)) {
        continue;
      }
      (void)mesh;
    }

    VkrMaterialHandle material_handle = draw->material;
    if (material_handle.id == 0) {
      material_handle =
          asset_submesh ? asset_submesh->material : mesh_submesh->material;
    }

    VkrMaterial *material = vkr_material_system_get_by_handle(
        &rf->material_system, material_handle);
    if (!material && rf->material_system.default_material.id != 0) {
      material = vkr_material_system_get_by_handle(
          &rf->material_system, rf->material_system.default_material);
    }
    if (!material) {
      continue;
    }

    VkrPipelineHandle pipeline = VKR_PIPELINE_HANDLE_INVALID;
    if (!vkr_pass_packet_resolve_pipeline(rf, VKR_PIPELINE_DOMAIN_UI, material,
                                          draw->pipeline_override, &pipeline)) {
      continue;
    }

    VkrRendererError refresh_err = VKR_RENDERER_ERROR_NONE;
    if (is_instance) {
      if (!vkr_mesh_manager_instance_refresh_pipeline(
              &rf->mesh_manager, draw->mesh, draw->submesh_index, pipeline,
              &refresh_err)) {
        continue;
      }
    } else {
      uint32_t mesh_index = draw->mesh.id - 1u;
      if (!vkr_mesh_manager_refresh_pipeline(&rf->mesh_manager, mesh_index,
                                             draw->submesh_index, pipeline,
                                             &refresh_err)) {
        continue;
      }
    }

    if (pipeline.id != globals_pipeline.id ||
        pipeline.generation != globals_pipeline.generation) {
      VkrRendererError bind_err = VKR_RENDERER_ERROR_NONE;
      if (!vkr_pipeline_registry_bind_pipeline(&rf->pipeline_registry, pipeline,
                                               &bind_err)) {
        continue;
      }

      vkr_material_system_apply_global(&rf->material_system,
                                       (VkrGlobalMaterialState *)globals,
                                       VKR_PIPELINE_DOMAIN_UI);
      globals_pipeline = pipeline;
    }

    uint32_t instance_id = VKR_INVALID_ID;
    if (is_instance) {
      instance_id = inst_state ? inst_state->instance_state.id : VKR_INVALID_ID;
    } else {
      instance_id =
          mesh_submesh ? mesh_submesh->instance_state.id : VKR_INVALID_ID;
    }
    vkr_shader_system_bind_instance(&rf->shader_system, instance_id);

    VkrPassDrawRange range = {0};
    if (asset_submesh) {
      if (!vkr_pass_packet_resolve_draw_range(rf, asset_submesh, false_v,
                                              &range)) {
        continue;
      }
    } else {
      if (!vkr_pass_packet_resolve_draw_range_mesh(rf, mesh_submesh, false_v,
                                                   &range)) {
        continue;
      }
    }

    VkrGeometryHandle geometry =
        asset_submesh ? asset_submesh->geometry : mesh_submesh->geometry;

    for (uint32_t inst = 0; inst < draw->instance_count; ++inst) {
      uint32_t instance_index = draw->first_instance + inst;
      if (instance_index >= payload->instance_count) {
        break;
      }

      const VkrInstanceDataGPU *inst_data = &payload->instances[instance_index];
      VkrLocalMaterialState local = {
          .model = inst_data->model,
          .object_id = inst_data->object_id,
      };
      vkr_material_system_apply_local(&rf->material_system, &local);
      vkr_material_system_apply_instance(&rf->material_system, material,
                                         VKR_PIPELINE_DOMAIN_UI);

      vkr_geometry_system_render_instanced_range_with_index_buffer(
          rf, &rf->geometry_system, geometry, range.index_buffer,
          range.index_count, range.first_index, range.vertex_offset, 1,
          base_instance + instance_index);
    }
  }
}

vkr_internal void vkr_pass_editor_execute(VkrRgPassContext *ctx,
                                          void *user_data) {
  (void)user_data;

  if (!ctx || !ctx->renderer) {
    return;
  }

  RendererFrontend *rf = (RendererFrontend *)ctx->renderer;
  const VkrRenderPacket *packet = vkr_rg_pass_get_packet(ctx);
  const VkrEditorPassPayload *payload = vkr_rg_pass_get_editor_payload(ctx);
  if (!packet || !payload) {
    return;
  }

  if (rf->editor_viewport.material.id != 0 && rf->offscreen_color_handles &&
      ctx->image_index < rf->offscreen_color_handle_count) {
    VkrMaterial *viewport_material = vkr_material_system_get_by_handle(
        &rf->material_system, rf->editor_viewport.material);
    if (viewport_material) {
      viewport_material->textures[VKR_TEXTURE_SLOT_DIFFUSE].handle =
          rf->offscreen_color_handles[ctx->image_index];
      viewport_material->textures[VKR_TEXTURE_SLOT_DIFFUSE].enabled = true_v;
    }
  }

  uint32_t base_instance = 0;
  if (!vkr_pass_packet_upload_instances(
          rf, payload->instances, payload->instance_count, &base_instance)) {
    return;
  }

  uint32_t width = packet->frame.viewport_width;
  uint32_t height = packet->frame.viewport_height;
  if (width == 0 || height == 0) {
    width = packet->frame.window_width;
    height = packet->frame.window_height;
  }
  if (width == 0 || height == 0) {
    return;
  }

  VkrGlobalMaterialState globals = {
      .projection = packet->globals.projection,
      .view = packet->globals.view,
      .ui_view = mat4_identity(),
      .ui_projection = mat4_ortho(0.0f, (float32_t)width, (float32_t)height,
                                  0.0f, -1.0f, 1.0f),
      .ambient_color = packet->globals.ambient_color,
      .view_position = packet->globals.view_position,
      .render_mode = (VkrRenderMode)packet->globals.render_mode,
  };

  vkr_pass_editor_draw_list(rf, payload, payload->draws, payload->draw_count,
                            base_instance, &globals);
}

bool8_t vkr_pass_editor_register(VkrRgExecutorRegistry *registry) {
  if (!registry) {
    return false_v;
  }

  VkrRgPassExecutor entry = {
      .name = string8_lit("pass.editor"),
      .execute = vkr_pass_editor_execute,
      .user_data = NULL,
  };

  return vkr_rg_executor_registry_register(registry, &entry);
}
