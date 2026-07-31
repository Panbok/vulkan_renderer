#include "renderer/passes/internal/vkr_pass_draw_utils.h"

#include <stdio.h>

#include "core/logger.h"
#include "renderer/systems/vkr_geometry_system.h"

/**
 * @brief Resolve the effective indexed draw range and optional opaque index
 * buffer override.
 *
 * The caller supplies both base and opaque index metadata because this helper
 * is shared by asset-backed and non-instanced mesh submesh types.
 */
static VkrPassDrawRange vkr_pass_packet_build_draw_range(
    RendererFrontend *rf, VkrGeometryHandle geometry, uint32_t index_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t opaque_index_count,
    uint32_t opaque_first_index, int32_t opaque_vertex_offset,
    bool8_t allow_opaque) {
  VkrPassDrawRange range = {
      .index_buffer = NULL,
      .index_count = index_count,
      .first_index = first_index,
      .vertex_offset = vertex_offset,
  };

  if (range.index_count == 0) {
    range.index_count = UINT32_MAX;
    range.first_index = 0;
    range.vertex_offset = 0;
  }

  if (allow_opaque && opaque_index_count > 0) {
    VkrGeometry *resolved =
        vkr_geometry_system_get_by_handle(&rf->geometry_system, geometry);
    if (resolved && resolved->opaque_index_buffer.handle) {
      range.index_buffer = &resolved->opaque_index_buffer;
      range.index_count = opaque_index_count;
      range.first_index = opaque_first_index;
      range.vertex_offset = opaque_vertex_offset;
    }
  }

  return range;
}

bool8_t vkr_pass_packet_upload_instances(RendererFrontend *rf,
                                         const VkrInstanceDataGPU *instances,
                                         uint32_t instance_count,
                                         uint32_t *out_base_instance) {
  if (!rf || !out_base_instance) {
    return false_v;
  }

  *out_base_instance = 0;
  if (instance_count == 0) {
    return true_v;
  }
  if (!instances) {
    return false_v;
  }
  if (!rf->instance_buffer_pool.initialized) {
    log_error("Instance buffer pool is not initialized");
    return false_v;
  }

  VkrInstanceDataGPU *dst = NULL;
  if (!vkr_instance_buffer_alloc(&rf->instance_buffer_pool, instance_count,
                                 out_base_instance, &dst)) {
    return false_v;
  }

  MemCopy(dst, instances,
          sizeof(VkrInstanceDataGPU) * (uint64_t)instance_count);
  vkr_instance_buffer_flush_range(&rf->instance_buffer_pool, *out_base_instance,
                                  instance_count);
  return true_v;
}

bool8_t vkr_pass_packet_resolve_instance(
    RendererFrontend *rf, VkrMeshHandle mesh, uint32_t submesh_index,
    VkrMeshInstance **out_instance, VkrMeshAsset **out_asset,
    VkrMeshAssetSubmesh **out_submesh,
    VkrMeshSubmeshInstanceState **out_instance_state) {
  if (!rf || mesh.id == 0 || !out_instance || !out_asset || !out_submesh ||
      !out_instance_state) {
    return false_v;
  }

  VkrMeshInstance *instance =
      vkr_mesh_manager_get_instance(&rf->mesh_manager, mesh);
  if (!instance || !instance->visible ||
      instance->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    return false_v;
  }

  VkrMeshAsset *asset =
      vkr_mesh_manager_get_asset(&rf->mesh_manager, instance->asset);
  if (!asset || submesh_index >= asset->submeshes.length) {
    return false_v;
  }

  VkrMeshAssetSubmesh *submesh =
      array_get_VkrMeshAssetSubmesh(&asset->submeshes, submesh_index);
  if (!submesh) {
    return false_v;
  }

  if (submesh_index >= instance->submesh_state.length) {
    return false_v;
  }

  VkrMeshSubmeshInstanceState *state = array_get_VkrMeshSubmeshInstanceState(
      &instance->submesh_state, submesh_index);
  if (!state) {
    return false_v;
  }

  *out_instance = instance;
  *out_asset = asset;
  *out_submesh = submesh;
  *out_instance_state = state;
  return true_v;
}

bool8_t vkr_pass_packet_handle_is_instance(VkrMeshHandle mesh) {
  return mesh.generation != 0;
}

bool8_t vkr_pass_packet_resolve_mesh(RendererFrontend *rf, VkrMeshHandle mesh,
                                     uint32_t submesh_index, VkrMesh **out_mesh,
                                     VkrSubMesh **out_submesh) {
  if (!rf || mesh.id == 0 || !out_mesh || !out_submesh) {
    return false_v;
  }

  if (mesh.generation != 0) {
    return false_v;
  }

  uint32_t mesh_index = mesh.id - 1u;
  VkrMesh *mesh_entry = vkr_mesh_manager_get(&rf->mesh_manager, mesh_index);
  if (!mesh_entry || !mesh_entry->visible ||
      mesh_entry->loading_state != VKR_MESH_LOADING_STATE_LOADED) {
    return false_v;
  }

  VkrSubMesh *submesh = vkr_mesh_manager_get_submesh(&rf->mesh_manager,
                                                     mesh_index, submesh_index);
  if (!submesh) {
    return false_v;
  }

  *out_mesh = mesh_entry;
  *out_submesh = submesh;
  return true_v;
}

bool8_t vkr_pass_packet_resolve_draw_range(RendererFrontend *rf,
                                           const VkrMeshAssetSubmesh *submesh,
                                           bool8_t allow_opaque,
                                           VkrPassDrawRange *out_range) {
  if (!rf || !submesh || !out_range) {
    return false_v;
  }

  *out_range = vkr_pass_packet_build_draw_range(
      rf, submesh->geometry, submesh->index_count, submesh->first_index,
      submesh->vertex_offset, submesh->opaque_index_count,
      submesh->opaque_first_index, submesh->opaque_vertex_offset, allow_opaque);
  return true_v;
}

bool8_t vkr_pass_packet_resolve_draw_range_mesh(RendererFrontend *rf,
                                                const VkrSubMesh *submesh,
                                                bool8_t allow_opaque,
                                                VkrPassDrawRange *out_range) {
  if (!rf || !submesh || !out_range) {
    return false_v;
  }

  *out_range = vkr_pass_packet_build_draw_range(
      rf, submesh->geometry, submesh->index_count, submesh->first_index,
      submesh->vertex_offset, submesh->opaque_index_count,
      submesh->opaque_first_index, submesh->opaque_vertex_offset, allow_opaque);
  return true_v;
}

const char *
vkr_pass_packet_default_shader_for_domain(VkrPipelineDomain domain) {
  switch (domain) {
  case VKR_PIPELINE_DOMAIN_UI:
    return "shader.default.ui";
  case VKR_PIPELINE_DOMAIN_SKYBOX:
    return "shader.default.skybox";
  case VKR_PIPELINE_DOMAIN_SHADOW:
    return "shader.shadow.opaque";
  case VKR_PIPELINE_DOMAIN_PICKING:
  case VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT:
  case VKR_PIPELINE_DOMAIN_PICKING_OVERLAY:
    return "shader.picking";
  case VKR_PIPELINE_DOMAIN_WORLD:
  case VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT:
  case VKR_PIPELINE_DOMAIN_WORLD_OVERLAY:
  default:
    return "shader.default.world";
  }
}

bool8_t vkr_pass_packet_resolve_pipeline(RendererFrontend *rf,
                                         VkrPipelineDomain domain,
                                         const VkrMaterial *material,
                                         VkrPipelineHandle pipeline_override,
                                         VkrPipelineHandle *out_pipeline) {
  if (!rf || !out_pipeline) {
    return false_v;
  }

  const char *fallback = vkr_pass_packet_default_shader_for_domain(domain);
  const char *pipeline_shader = fallback;
  bool8_t has_material_shader = false_v;
  if (material && material->shader_name && material->shader_name[0] != '\0') {
    bool8_t allow_material_shader = false_v;
    if (domain == VKR_PIPELINE_DOMAIN_WORLD) {
      allow_material_shader =
          (material->pipeline_id == VKR_INVALID_ID) ||
          (material->pipeline_id == VKR_PIPELINE_DOMAIN_WORLD);
    } else if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT) {
      allow_material_shader =
          (material->pipeline_id == VKR_INVALID_ID) ||
          (material->pipeline_id == VKR_PIPELINE_DOMAIN_WORLD) ||
          (material->pipeline_id == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT);
    } else if (domain == VKR_PIPELINE_DOMAIN_WORLD_OVERLAY) {
      allow_material_shader =
          (material->pipeline_id == VKR_INVALID_ID) ||
          (material->pipeline_id == VKR_PIPELINE_DOMAIN_WORLD) ||
          (material->pipeline_id == VKR_PIPELINE_DOMAIN_WORLD_OVERLAY);
    } else {
      allow_material_shader = (material->pipeline_id == (uint32_t)domain);
    }
    if (allow_material_shader) {
      pipeline_shader = material->shader_name;
      has_material_shader = true_v;
    }
  }

  if (pipeline_override.id != 0) {
    VkrPipeline *pipeline = NULL;
    if (vkr_pipeline_registry_get_pipeline(&rf->pipeline_registry,
                                           pipeline_override, &pipeline) &&
        pipeline && pipeline->domain == domain) {
      *out_pipeline = pipeline_override;
    } else {
      pipeline_override = VKR_PIPELINE_HANDLE_INVALID;
    }
  }

  if (pipeline_override.id == 0) {
    char domain_shader_name[256] = {0};
    const char *lookup_candidates[3] = {0};
    uint32_t lookup_count = 0;
    if (has_material_shader) {
      if (domain == VKR_PIPELINE_DOMAIN_WORLD_TRANSPARENT) {
        int written = snprintf(domain_shader_name, sizeof(domain_shader_name),
                               "%s.transparent", pipeline_shader);
        if (written > 0 && (size_t)written < sizeof(domain_shader_name)) {
          lookup_candidates[lookup_count++] = domain_shader_name;
        }
      } else if (domain == VKR_PIPELINE_DOMAIN_WORLD_OVERLAY) {
        int written = snprintf(domain_shader_name, sizeof(domain_shader_name),
                               "%s.overlay", pipeline_shader);
        if (written > 0 && (size_t)written < sizeof(domain_shader_name)) {
          lookup_candidates[lookup_count++] = domain_shader_name;
        }
      }
      lookup_candidates[lookup_count++] = pipeline_shader;
    }
    lookup_candidates[lookup_count++] = NULL;

    VkrRendererError err = VKR_RENDERER_ERROR_NONE;
    bool8_t resolved = false_v;
    for (uint32_t i = 0; i < lookup_count; ++i) {
      const char *lookup_name = lookup_candidates[i];
      if (vkr_pipeline_registry_get_pipeline_for_material(
              &rf->pipeline_registry, lookup_name, (uint32_t)domain,
              out_pipeline, &err)) {
        resolved = true_v;
        break;
      }
    }
    if (!resolved) {
      log_error(
          "Failed to resolve pipeline for material '%s' in domain %u: error %d",
          pipeline_shader ? pipeline_shader : "(null)", (uint32_t)domain, err);
      return false_v;
    }
  }

  VkrPipeline *resolved_pipeline = NULL;
  vkr_pipeline_registry_get_pipeline(&rf->pipeline_registry, *out_pipeline,
                                     &resolved_pipeline);

  if (resolved_pipeline && resolved_pipeline->shader_name.str &&
      resolved_pipeline->shader_name.length > 0 &&
      resolved_pipeline->shader_name
              .str[resolved_pipeline->shader_name.length] == '\0') {
    if (vkr_shader_system_use(
            &rf->shader_system,
            (const char *)resolved_pipeline->shader_name.str)) {
      return true_v;
    }
  }

  const char *resolved_shader = fallback;
  if (pipeline_shader && pipeline_shader[0] != '\0' && resolved_pipeline &&
      resolved_pipeline->domain == domain) {
    resolved_shader = pipeline_shader;
  }

  if (!vkr_shader_system_use(&rf->shader_system, resolved_shader)) {
    if (resolved_shader == fallback ||
        !vkr_shader_system_use(&rf->shader_system, fallback)) {
      return false_v;
    }
  }

  return true_v;
}

bool8_t vkr_pass_indirect_batch_submit(RendererFrontend *rf,
                                       VkrPassIndirectBatch *batch) {
  if (!rf || !batch || batch->count == 0) {
    if (batch) {
      batch->count = 0;
    }
    return false_v;
  }

  bool8_t used_indirect = false_v;
  if (batch->count > 1u && rf->supports_multi_draw_indirect &&
      rf->indirect_draw_system.initialized &&
      rf->indirect_draw_system.enabled) {
    // firstInstance inside an indirect command needs its own device feature.
    bool8_t needs_first_instance = false_v;
    for (uint32_t i = 0; i < batch->count; ++i) {
      if (batch->commands[i].first_instance != 0u) {
        needs_first_instance = true_v;
        break;
      }
    }

    if (!needs_first_instance || rf->supports_draw_indirect_first_instance) {
      uint32_t base_draw = 0;
      VkrIndirectDrawCommand *dst = NULL;
      if (vkr_indirect_draw_alloc(&rf->indirect_draw_system, batch->count,
                                  &base_draw, &dst)) {
        MemCopy(dst, batch->commands,
                sizeof(VkrIndirectDrawCommand) * (uint64_t)batch->count);
        vkr_indirect_draw_flush_range(&rf->indirect_draw_system, base_draw,
                                      batch->count);
        VkrBufferHandle indirect_buffer =
            vkr_indirect_draw_get_current(&rf->indirect_draw_system);
        if (indirect_buffer) {
          vkr_geometry_system_render_indirect(
              rf, &rf->geometry_system, batch->geometry, indirect_buffer,
              (uint64_t)base_draw * sizeof(VkrIndirectDrawCommand),
              batch->count, (uint32_t)sizeof(VkrIndirectDrawCommand));
          rf->frame_metrics.world.indirect_draws_issued += batch->count;
          used_indirect = true_v;
        }
      }
    }
  }

  if (!used_indirect) {
    for (uint32_t i = 0; i < batch->count; ++i) {
      const VkrIndirectDrawCommand *cmd = &batch->commands[i];
      vkr_geometry_system_render_instanced_range_with_index_buffer(
          rf, &rf->geometry_system, batch->geometry, batch->index_buffer,
          cmd->index_count, cmd->first_index, cmd->vertex_offset,
          cmd->instance_count, cmd->first_instance);
    }
  }

  batch->count = 0;
  return used_indirect;
}
